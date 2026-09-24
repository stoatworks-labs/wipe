/**
 * Wipe (WP01) — the browser demo.
 *
 * Wipe is an FFGL **mixer**: Resolume hands it two textures, the layer below
 * (A, `inputTextures[0]`) and this layer (B, `inputTextures[1]`), and the
 * layer's opacity fader. It is the first mixer in the demo suite, and the kit
 * hands every demo exactly one input, so this page generates the second one
 * itself: A is the kit's clip (its "Clip" dropdown, relabelled "Clip A", and
 * the only one "Use my own…" replaces), B is a second generated clip from the
 * kit's own `sources.js`, picked from the transport's one extra dropdown. Both
 * are functions of (uv, time) and neither is footage.
 *
 * What runs for real, and what is a port:
 *
 *   - **The shaders are the plugin's**, `kVertexShader` and `kWipeShader` from
 *     source/Shaders.cpp, copied across unedited. `demo/tools/check_shaders.py`
 *     compares them character for character and `tools/verify.sh` runs it.
 *     The one escape is the backtick around `soft` in a comment, which a
 *     template literal cannot hold raw; the checker decodes it and refuses any
 *     other backslash.
 *   - **The CPU half is a hand port**: waveform.js holds Controls.cpp (every
 *     `...FromParam`), Waveform.cpp (`Derive`, `EdgeLevel`, and the whole
 *     Area law: the half-plane, polygon, disc and wedge clipping, the lattice
 *     preparation, the adaptive Simpson band integral and the bisection) and
 *     Timing.cpp's `ModPhase`; this file holds the Flip-Flop and area-cache
 *     logic of `Wipe::ProcessOpenGL`. Nothing checks that port but a reader.
 *     A lattice with the soft edge up is solved in area-worker.js rather
 *     than in line (see "the level" below). The
 *     matrix's Feistel order is NOT ported: it runs in the shader only, as it
 *     does in the plugin, and the CPU's matrix area is a count of ranks.
 *   - **Everything else is not the plugin**: no Resolume, no layer stack, no
 *     FFGL, GLSL ES 3.00 rather than 4.1 core. The textures are not padded, so
 *     both MaxUVs are 1 — the one thing a mixer's per-input MaxUV exists for
 *     cannot be shown here, and the page says so.
 *
 * Opacity is a slider on this page. In Resolume it is not: a mixer parameter
 * named Opacity is bound to the LAYER's opacity fader (measured on genlock and
 * on Wipe in Arena 7.27.1), so the fader drives the wipe and the mixer's own
 * Opacity control is overridden. The page says that too.
 */

import { mountDemo } from './vendor/demo.js';
import { Program } from './vendor/gl.js';
import { SOURCES, SourceRenderer } from './vendor/sources.js';
import {
  f32, clamp01, clamp, PX_MAX, MULTIPLE_MAX,
  PAT_HORIZONTAL, PAT_BOX, PAT_DIAMOND, PAT_CIRCLE, PAT_CLOCK, PAT_MATRIX, PAT_COUNT,
  LAW_AREA, LAW_COUNT,
  softnessPxFromParam, borderWidthPxFromParam, borderSoftnessPxFromParam, rotationRadiansFromParam,
  aspectFromParam, modAmountPxFromParam, modFrequencyFromParam, modSpeedHzFromParam, multipleFromParam,
  optionIndex, derive, prepare, edgeLevel, softArea, areaLevel, modPhase,
} from './waveform.js';

//---------------------------------------------------------------------------
// The plugin's GLSL, from source/Shaders.cpp. DO NOT EDIT HERE: change the
// C++ and copy it across; demo/tools/check_shaders.py fails verify otherwise.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const WIPE = `#version 410 core

//inputTextures[0] -- Dest, the layer BELOW: A.
uniform sampler2D TextureA;
//inputTextures[1] -- Src, THIS layer: B, the picture the fader wipes in.
uniform sampler2D TextureB;

//Each input has its own. They are not the same number and there is no
//circumstance in which using one for the other is safe.
uniform vec2 MaxUVA;
uniform vec2 MaxUVB;
uniform vec2 HalfTexelA;
uniform vec2 HalfTexelB;

//The fader: the Opacity parameter, which Resolume drives from the layer's
//opacity fader. 0 is A, 1 is B.
uniform float Position;

//The generator's frame -- see Waveform.h.
uniform float Pattern;   //0 horizontal, 1 vertical, 2 box, 3 diamond, 4 circle, 5 clock, 6 matrix
uniform float Reverse;   //the EFFECTIVE reverse, flip-flop already folded in
uniform vec2 Centre;
uniform float ScaleX;
uniform vec2 RotCS;      //cos, sin of the rotation
uniform float Norm;      //R
uniform vec2 Multiple;   //Multiple H, Multiple V
uniform vec2 WRange;     //the waveform's min and max over the picture
uniform vec2 MatrixCells;//columns, rows

//The comparators, all in W units (converted on the CPU from pixels).
uniform float Level;
uniform float SoftW;
uniform float BorderW;
uniform float BorderSoftW;
uniform vec3 BorderColour;

//The modulator.
uniform float ModW;
uniform float ModFreq;
uniform float ModPhase;//cycles, already reduced into [0,1) on the CPU

in vec2 uv;
out vec4 fragColor;

const float kTwoPi = 6.283185307179586;

//Clamped half a texel inside the used area: GL_LINEAR at the boundary takes
//half its weight from the texture's undrawn padding.
vec4 fetchA( vec2 p )
{
	vec2 q = clamp( p, HalfTexelA, vec2( 1.0 ) - HalfTexelA );
	return texture( TextureA, q * MaxUVA );
}

vec4 fetchB( vec2 p )
{
	vec2 q = clamp( p, HalfTexelB, vec2( 1.0 ) - HalfTexelB );
	return texture( TextureB, q * MaxUVB );
}

//---------------------------------------------------------------- the matrix
//A bijection on [0, count): a Feistel network over the next power of two,
//walked until it lands inside the count. Integer for integer the same as
//MatrixRank in Waveform.cpp, so the CPU's area of "cells below the level"
//is exactly what this draws.
uint mix32( uint x )
{
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

uint feistel( uint x, uint halfBits )
{
	uint mask = ( 1u << halfBits ) - 1u;
	uint l = x >> halfBits;
	uint r = x & mask;
	for( uint rnd = 0u; rnd < 4u; ++rnd )
	{
		uint f = mix32( r + rnd * 0x9E3779B9u ) & mask;
		uint t = r;
		r = l ^ f;
		l = t;
	}
	return ( l << halfBits ) | r;
}

float matrixRank( vec2 p )
{
	uint cols = uint( MatrixCells.x );
	uint rows = uint( MatrixCells.y );
	uint count = cols * rows;
	uint cx = min( uint( floor( p.x * MatrixCells.x ) ), cols - 1u );
	uint cy = min( uint( floor( p.y * MatrixCells.y ) ), rows - 1u );
	uint index = cy * cols + cx;
	uint bits = 2u;
	while( ( 1u << bits ) < count )
		bits += 2u;
	uint x = index;
	for( int walk = 0; walk < 64; ++walk )
	{
		x = feistel( x, bits / 2u );
		if( x < count )
			break;
	}
	return ( float( x ) + 0.5 ) / float( count );
}

//-------------------------------------------------------------- the waveform
//A ramp that wraps n times across the picture, centred so every copy has
//its own zero: the "multiple" switch on a pattern generator.
float cper( float u, float n )
{
	return n < 1.5 ? u : ( fract( n * u + 0.5 ) - 0.5 ) / n;
}

float waveform( vec2 p )
{
	vec2 d = p - Centre;
	d.x *= ScaleX;
	vec2 q = vec2( RotCS.x * d.x - RotCS.y * d.y, RotCS.y * d.x + RotCS.x * d.y );

	float w;
	if( Pattern < 0.5 )
		w = Multiple.x < 1.5 ? q.x + 0.5 : fract( Multiple.x * ( q.x + 0.5 ) );
	else if( Pattern < 1.5 )
		w = Multiple.y < 1.5 ? q.y + 0.5 : fract( Multiple.y * ( q.y + 0.5 ) );
	else if( Pattern < 4.5 )
	{
		float h = cper( q.x, Multiple.x );
		float v = cper( q.y, Multiple.y );
		if( Pattern < 2.5 )
			w = max( abs( h ), abs( v ) ) / Norm;      //NAM of the two ramps
		else if( Pattern < 3.5 )
			w = ( abs( h ) + abs( v ) ) / Norm;        //the two ramps added
		else
			w = ( h * h + v * v ) / ( Norm * Norm );   //the two parabolas added
	}
	else if( Pattern < 5.5 )
	{
		//Clockwise from twelve. atan( x, y ) is 0 straight up and pi/2 at
		//three o'clock.
		float a = atan( q.x, q.y ) / kTwoPi;
		w = fract( Multiple.x * a );
	}
	else
		w = matrixRank( p );

	if( Reverse > 0.5 )
		w = WRange.x + WRange.y - w;

	//The modulator: a sine added to the waveform, along the edge. Down the
	//picture for every pattern but the vertical wipe, whose edge runs across.
	float along = ( Pattern > 0.5 && Pattern < 1.5 ) ? p.x : p.y;
	w += ModW * sin( kTwoPi * ( ModFreq * along - ModPhase ) );
	return w;
}

//------------------------------------------------------------ the comparator
//A limited-gain comparator: linear between its rails, over a band \`soft\`
//wide in W, centred on the level. 1 where the waveform is below the level.
float compare( float w, float level, float soft )
{
	if( soft <= 0.0 )
		return w < level ? 1.0 : 0.0;
	return clamp( 0.5 + ( level - w ) / soft, 0.0, 1.0 );
}

void main()
{
	//The fader's ends are pure fetches. At 0 the layer below is untouched;
	//at 1 this layer is. Nothing else is evaluated there, so neither a soft
	//edge nor a border can leak into an end stop.
	if( Position <= 0.0 )
	{
		fragColor = fetchA( uv );
		return;
	}
	if( Position >= 1.0 )
	{
		fragColor = fetchB( uv );
		return;
	}

	float w = waveform( uv );

	float key = compare( w, Level, SoftW );

	//The border is a second comparator, a little above the level, and the
	//border is where the two disagree. It is OUT OF CIRCUIT at zero width:
	//two comparators of different gain at the same level disagree across
	//the whole soft edge, and a border width of nothing must mean no
	//border, not a hard comparator's opinion of a soft one. Found by
	//--edge, as a 0.2 px shift of every soft edge towards A.
	float border = 0.0;
	if( BorderW > 0.0 )
	{
		float outer = compare( w, Level + BorderW, BorderSoftW );
		border = max( outer - key, 0.0 );
	}

	vec4 a = fetchA( uv );
	vec4 b = fetchB( uv );
	vec4 mixed = mix( a, b, key );
	fragColor = mix( mixed, vec4( BorderColour, 1.0 ), border );
}
`;
//===========================================================================
// The parameters, in Wipe.h's ParamID order, with Wipe::Wipe()'s names,
// groups, types, elements and defaults.
//===========================================================================
const PATTERN_NAMES = ['Horizontal', 'Vertical', 'Box', 'Diamond', 'Circle', 'Clock', 'Matrix'];
const LAW_NAMES = ['Edge', 'Area'];

// FF_TYPE_INTEGER, 1..8. The kit has no integer control, so these are
// dropdowns of the plugin's own integers, as galvo's are. The dropdown's index
// is NOT the plugin's value: `multipleOf` adds the 1 back.
const MULTIPLE_ELEMENTS = Array.from({ length: MULTIPLE_MAX }, (_, i) => String(i + 1));
const multipleOf = (index) => multipleFromParam(Math.round(index) + 1);

const px = (v) => `${(clamp01(v) * PX_MAX).toFixed(1)} px`;

const PARAMS = [
  // Index 0, which Resolume Arena does not show for a mixer: on, for ever.
  { id: 'aspectComp', name: 'Aspect Comp', type: 'boolean', default: 1, group: 'Pattern',
    hint: 'Scales the pattern’s horizontal axis by the picture’s aspect, so a circle is round rather than the ellipse an uncompensated generator drew. Index 0: Resolume Arena does not show a mixer’s first parameter, so in Arena this is on and stays on.' },
  { id: 'pattern', name: 'Pattern', type: 'option', elements: PATTERN_NAMES, default: PAT_HORIZONTAL, group: 'Pattern',
    hint: 'Which waveform the comparator sees: the two ramps, the two ramps combined by NAM (box) or added (diamond), the two parabolas added (circle), the angle (clock), or a per-cell rank (matrix).' },
  { id: 'reverse', name: 'Reverse', type: 'boolean', default: 0, group: 'Pattern',
    hint: 'Mirrors the waveform within its own range: a box closes instead of opening, a ramp runs the other way.' },
  { id: 'flipFlop', name: 'Flip-Flop', type: 'boolean', default: 0, group: 'Pattern',
    hint: 'Each arrival of the fader at one end from the other flips the direction, so the next transition runs back the way it came. Drag Opacity all the way to 1, then all the way to 0, to see it.' },

  { id: 'opacity', name: 'Opacity', type: 'standard', default: 0.5, group: 'Fader',
    display: (v) => `${(clamp01(v) * 100).toFixed(1)}% B`,
    hint: 'The fader: 0 is A, the layer below, 1 is B, this layer. In Resolume this is bound to the LAYER’s opacity fader and the mixer’s own control is overridden; here it is a slider.' },
  { id: 'law', name: 'Law', type: 'option', elements: LAW_NAMES, default: 0, group: 'Fader',
    hint: 'Edge: the hardware’s law, the comparator level linear in the fader. Area: the level solved so the (soft) B area is exactly the fader. The line under the picture shows both.' },

  { id: 'softness', name: 'Softness', type: 'standard', default: 0, group: 'Edge', display: px,
    hint: 'The comparator’s gain, in output pixels measured on the horizontal ramp. Every other pattern’s edge follows its own slope from there, so a circle is softer near its middle.' },
  { id: 'borderWidth', name: 'Border Width', type: 'standard', default: 0, group: 'Edge', display: px,
    hint: 'A second comparator this far above the level; the border is where the two disagree. At zero it is out of circuit.' },
  { id: 'borderSoftness', name: 'Border Softness', type: 'standard', default: 0, group: 'Edge', display: px },
  { id: 'borderR', name: 'Border Red', type: 'colour', default: 1, group: 'Edge' },
  { id: 'borderG', name: 'Border Green', type: 'colour', default: 1, group: 'Edge' },
  { id: 'borderB', name: 'Border Blue', type: 'colour', default: 1, group: 'Edge' },

  { id: 'centreX', name: 'Centre X', type: 'standard', default: 0.5, group: 'Positioner',
    hint: 'The positioner. Box, Diamond, Circle and Clock only: the ramps and the matrix ignore it, as the hardware’s did.' },
  { id: 'centreY', name: 'Centre Y', type: 'standard', default: 0.5, group: 'Positioner' },
  { id: 'rotation', name: 'Rotation', type: 'standard', default: 0, group: 'Positioner',
    display: (v) => `${(rotationRadiansFromParam(v) * 180 / Math.PI).toFixed(1)}°` },
  { id: 'aspect', name: 'Aspect', type: 'standard', default: 0.5, group: 'Positioner',
    display: (v) => `×${aspectFromParam(v).toFixed(3)}`,
    hint: '0.25 to 4, geometric, unity in the middle. Multiplies the pattern’s horizontal coordinate: above unity the pattern is narrower.' },

  { id: 'modAmount', name: 'Mod Amount', type: 'standard', default: 0, group: 'Modulation', display: px,
    hint: 'A sine added to the waveform, down the picture (across it for the vertical wipe). Its amplitude, in pixels on the horizontal ramp.' },
  { id: 'modFrequency', name: 'Mod Frequency', type: 'standard', default: 0.4, group: 'Modulation',
    display: (v) => `${modFrequencyFromParam(v).toFixed(2)} cycles` },
  { id: 'modSpeed', name: 'Mod Speed', type: 'standard', default: 0.25, group: 'Modulation',
    display: (v) => `${modSpeedHzFromParam(v).toFixed(2)} Hz` },
  { id: 'multipleH', name: 'Multiple H', type: 'option', elements: MULTIPLE_ELEMENTS, default: 0, group: 'Modulation',
    hint: 'FF_TYPE_INTEGER, 1 to 8: how many times the horizontal ramp wraps across the picture. With Aspect Comp on it counts per picture height of width. A dropdown here because the page has no integer control.' },
  { id: 'multipleV', name: 'Multiple V', type: 'option', elements: MULTIPLE_ELEMENTS, default: 0, group: 'Modulation',
    hint: 'FF_TYPE_INTEGER, 1 to 8. The matrix multiplies its 32 × 18 grid by these two.' },
];

//===========================================================================
// B, the second input. The kit renders A; this renders B from the same
// generated clips, at the same raster and the same clock.
//===========================================================================
const B_CLIPS = ['grid', 'scene', 'bars', 'ramp', 'spot', 'detail', 'alpha'];
const B_DEFAULT = 'grid';

//===========================================================================
// Wipe::ProcessOpenGL, as far as a browser has it.
//===========================================================================
const stats = { text: '' };

// Filled in once the page has mounted: a worker's answer asks for a frame even
// while the transport is paused.
let redraw = () => {};

function createRenderer(gl, quad) {
  const program = new Program(gl, VERTEX, WIPE, 'wipe');
  const clipB = new SourceRenderer(gl, quad);

  // The plugin's per-instance state: Flip-Flop's memory and the Area cache.
  let lastEnd = -1;
  let flipped = false;
  let transitions = 0;
  const areaCache = { key: null, level: 0.0 };
  const statCache = { key: null, area: 0.0 };

  // The worker for the one case too slow for a page's main thread. Only its
  // latest answer is used; `asked` is the key it is working on.
  let worker = null;
  let asked = null;
  const workerCache = { key: null, level: 0.0, area: 0.0 };
  try {
    worker = new Worker(new URL('./area-worker.js', import.meta.url), { type: 'module' });
    worker.onmessage = (event) => {
      Object.assign(workerCache, event.data);
      redraw();
    };
  } catch {
    worker = null; // then it is solved in line, as the plugin does, and the page stalls instead
  }

  return {
    render({ input, params, width, height, time, variant }) {
      const P = (id) => f32(params.get(id));

      //-------------------------------------------------------- the fader
      const position = clamp(P('opacity'), 0.0, 1.0);
      const flipflop = P('flipFlop') > 0.5;
      if (position >= 1.0) {
        if (lastEnd === 0) {
          transitions += 1;
          if (flipflop) flipped = !flipped;
        }
        lastEnd = 1;
      } else if (position <= 0.0) {
        if (lastEnd === 1) {
          transitions += 1;
          if (flipflop) flipped = !flipped;
        }
        lastEnd = 0;
      }
      if (!flipflop) flipped = false;

      //-------------------------------------------------------- the frame
      const frame = {
        pattern: optionIndex(P('pattern'), PAT_COUNT),
        reverse: (P('reverse') > 0.5) !== flipped,
        aspectComp: P('aspectComp') > 0.5,
        centreX: clamp(P('centreX'), 0, 1),
        centreY: clamp(P('centreY'), 0, 1),
        rotation: rotationRadiansFromParam(P('rotation')),
        aspect: aspectFromParam(P('aspect')),
        multipleH: multipleOf(params.get('multipleH')),
        multipleV: multipleOf(params.get('multipleV')),
        softnessPx: softnessPxFromParam(P('softness')),
        outW: width,
        outH: height,
      };
      const derived = derive(frame);

      const softW = frame.softnessPx * derived.unitsPerPixel;
      const borderW = borderWidthPxFromParam(P('borderWidth')) * derived.unitsPerPixel;
      const borderSoftW = borderSoftnessPxFromParam(P('borderSoftness')) * derived.unitsPerPixel;
      const modW = modAmountPxFromParam(P('modAmount')) * derived.unitsPerPixel;
      const modFreq = modFrequencyFromParam(P('modFrequency'));
      const phase = modPhase(time, modSpeedHzFromParam(P('modSpeed')));

      //-------------------------------------------------------- the level
      //
      // Edge law is linear in the fader. Area law is the plugin's bisection
      // over its closed-form soft area, cached on the same inputs the plugin
      // caches on -- in line, as the plugin does it, EXCEPT for a lattice
      // (Box, Diamond or Circle with a Multiple above 1) with the soft edge
      // up: that costs the port up to a couple of seconds a solve, so it goes
      // to area-worker.js and the page draws with the last level it has until
      // the answer arrives. The line under the picture says when that is so.
      const frameKey = JSON.stringify(frame);
      const areaLaw = optionIndex(P('law'), LAW_COUNT) === LAW_AREA;
      const lattice = (frame.pattern === PAT_BOX || frame.pattern === PAT_DIAMOND || frame.pattern === PAT_CIRCLE)
        && (frame.multipleH > 1 || frame.multipleV > 1);
      const offThread = worker !== null && lattice && softW > 0.0;

      let level;
      let area;
      let stale = false;
      const edge = edgeLevel(derived, position);

      if (offThread) {
        const key = `${frameKey}|${position}|${softW}|${areaLaw ? 'area' : edge}`;
        if (workerCache.key !== key) {
          stale = true;
          if (asked !== key) {
            asked = key;
            worker.postMessage({ key, frame, position, softW, areaLaw, level: edge });
          }
        }
        // The last answer the worker gave, for whatever it was asked last
        // time -- or, before it has ever answered, the Edge level.
        level = areaLaw ? (workerCache.key !== null ? workerCache.level : edge) : edge;
        area = workerCache.key !== null ? workerCache.area : NaN;
      } else {
        let pre = null;
        if (areaLaw) {
          const key = `${frameKey}|${position}|${softW}`;
          if (areaCache.key !== key) {
            pre = prepare(frame, derived);
            areaCache.level = areaLevel(frame, derived, pre, position, softW);
            areaCache.key = key;
          }
          level = areaCache.level;
        } else {
          level = edge;
        }

        // Not the plugin: the page reads the B area back out of the plugin's
        // own SoftArea at the level it chose, so the two laws can be told
        // apart by eye. Cached like the solve, because it is the same work.
        const statKey = `${frameKey}|${level}|${softW}`;
        if (statCache.key !== statKey) {
          statCache.area = softArea(frame, derived, pre ?? prepare(frame, derived), level, softW);
          statCache.key = statKey;
        }
        area = statCache.area;
      }

      const flip = flipflop ? `, direction ${flipped ? 'flipped' : 'normal'}` : '';
      if (position <= 0 || position >= 1) {
        stats.text = `Fader at an end: a pure fetch of ${position <= 0 ? 'A' : 'B'}, nothing else runs. Transitions ${transitions}${flip}.`;
      } else if (stale) {
        stats.text = `${PATTERN_NAMES[frame.pattern]} lattice, ${LAW_NAMES[areaLaw ? 1 : 0]} law · solving the soft area off the main thread — the picture holds the previous level until it lands · transitions ${transitions}${flip}`;
      } else {
        stats.text = `${PATTERN_NAMES[frame.pattern]}, ${LAW_NAMES[areaLaw ? 1 : 0]} law · level ${level.toFixed(4)} in W (range ${derived.wMin.toFixed(3)}…${derived.wMax.toFixed(3)}) · B area ${(area * 100).toFixed(2)}% for a fader at ${(position * 100).toFixed(2)}% · softness ${softW.toFixed(5)} W · transitions ${transitions}${flip}`;
      }

      //-------------------------------------------------------- the inputs
      const bClip = SOURCES.find((s) => s.id === (variant ?? B_DEFAULT)) ?? SOURCES[0];
      const b = clipB.render(bClip, width, height, time);

      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      gl.disable(gl.BLEND);

      program.use();
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, input.texture);
      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, b.texture);
      program.setSampler('TextureA', 0);
      program.setSampler('TextureB', 1);

      // Unpadded textures in a browser: both MaxUVs are exactly 1.
      program.set('MaxUVA', 1.0, 1.0);
      program.set('MaxUVB', 1.0, 1.0);
      program.set('HalfTexelA', f32(0.5 / input.width), f32(0.5 / input.height));
      program.set('HalfTexelB', f32(0.5 / b.width), f32(0.5 / b.height));

      program.set('Position', f32(position));
      program.set('Pattern', frame.pattern);
      program.set('Reverse', frame.reverse ? 1.0 : 0.0);
      program.set('Centre', derived.centreX, derived.centreY);
      program.set('ScaleX', derived.scaleX);
      program.set('RotCS', derived.cosR, derived.sinR);
      program.set('Norm', derived.norm);
      program.set('Multiple', frame.multipleH, frame.multipleV);
      program.set('WRange', derived.wMin, derived.wMax);
      program.set('MatrixCells', derived.matrixCols, derived.matrixRows);

      program.set('Level', level);
      program.set('SoftW', softW);
      program.set('BorderW', borderW);
      program.set('BorderSoftW', borderSoftW);
      program.set('BorderColour', P('borderR'), P('borderG'), P('borderB'));

      program.set('ModW', modW);
      program.set('ModFreq', modFreq);
      program.set('ModPhase', phase);

      quad.draw();

      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, null);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, null);
    },
  };
}

//===========================================================================
// The page.
//===========================================================================
const mounted = mountDemo({
  name: 'Wipe',
  // The FFGL type the plugin registers (PluginInfo), for the kit banner's
  // closing sentence, which said "effect" on every page until 2026-09-24.
  kind: 'mixer',
  pluginId: 'WP01',
  tagline:
    'A 1970s vision mixer’s analogue pattern generator, as an FFGL mixer. Every wipe is a waveform — ramps and parabolas combined in a small matrix — and a comparator against the fader. Softness is the comparator’s gain, so a circle’s edge is softer near its middle where the parabola is flatter; the border is a second comparator; modulation is a sine on the waveform.',
  repo: 'https://github.com/stoatworks-labs/wipe',
  page: 'https://stoatworks-labs.com/software/wipe/',
  video: 'https://www.youtube.com/watch?v=HIclO2s5ylw',

  // The stock sentence says "running on generated clips", which is true but
  // hides the one thing a visitor needs to know about a mixer: there are two.
  blurb:
    'It is Wipe’s own GLSL, ported from the repository to WebGL2, with the plugin’s CPU half — the pattern frame, both fader laws and the closed-form Area solve — ported to JavaScript. Wipe is a mixer, so it needs two pictures: A (the layer below) is the Clip A dropdown and B (this layer) is Clip B, both generated in this page. In Resolume the fader is the layer’s opacity fader; here it is the Opacity slider.',

  sources: ['bars', 'scene', 'grid', 'ramp', 'spot', 'detail', 'alpha'],

  // Not "which plugin" this time: Wipe ships one bundle. It is the one extra
  // transport dropdown the kit has, and B is transport, not a parameter the
  // plugin declares — which is exactly why it must not be in the inspector.
  variants: {
    label: 'Clip B',
    default: B_DEFAULT,
    options: B_CLIPS.map((id) => {
      const s = SOURCES.find((x) => x.id === id);
      return { id, name: s.name, hint: `B, this layer: ${s.hint}` };
    }),
  },

  params: PARAMS,

  presets: {
    'Soft circle, gold border': {
      pattern: PAT_CIRCLE, opacity: 0.42, softness: 0.25, borderWidth: 0.12, borderSoftness: 0.05,
      borderR: 1.0, borderG: 0.78, borderB: 0.2,
    },
    'Modulated wave': { pattern: PAT_HORIZONTAL, softness: 0.1, modAmount: 0.4, modFrequency: 0.45, modSpeed: 0.2 },
    'Box, Area law': { pattern: PAT_BOX, law: 1, opacity: 0.5, softness: 0.3 },
    'Clock from off-centre': { pattern: PAT_CLOCK, centreX: 0.35, centreY: 0.6, softness: 0.15, borderWidth: 0.06 },
    'Diamond lattice': { pattern: PAT_DIAMOND, multipleH: 3, multipleV: 1, softness: 0.1, opacity: 0.55 },
    'Matrix': { pattern: PAT_MATRIX, opacity: 0.45 },
    'Uncompensated circle': { aspectComp: 0, pattern: PAT_CIRCLE, opacity: 0.35 },
  },

  differences: [
    'Two inputs, both generated here. The kit this page is built on hands a demo one input; Wipe is the first mixer in the suite, so B is rendered by a second copy of the kit’s own clip generator at the same raster and on the same clock. “Use my own…” replaces A only.',
    'Opacity is a slider. In Resolume Arena a mixer parameter named Opacity is bound to the layer’s opacity fader (measured on Wipe in Arena 7.27.1): the fader drives the wipe and writes to the mixer’s own Opacity are overridden. Arena also hides a mixer’s first parameter, which is why Aspect Comp is first and on — here it is shown, because a browser does not hide it.',
    'Both MaxUVs are 1. Resolume hands a mixer two textures that are padded and usually of different sizes, and the shader applies each input’s own MaxUV — the arrangement the plugin’s --mixer check exists for. The page’s textures are unpadded, so that half of the shader runs with nothing to correct. A file of your own does arrive at its own size, so the two half-texel clamps differ then.',
    'The CPU half is a port, and nothing checks it but a reader. Controls.cpp, Waveform.cpp’s frame, Edge law and the closed-form Area law (the polygon, disc and wedge clipping, the lattice, the adaptive Simpson band integral and the bisection), Timing.cpp’s modulator phase and ProcessOpenGL’s Flip-Flop and cache are translated to JavaScript by hand. The shaders are checked: demo/tools/check_shaders.py fails the repository’s verify script if a character drifts from source/Shaders.cpp.',
    'Area law is solved in line and cached on the same inputs, as the plugin does, with one exception: a lattice (Box, Diamond or Circle with a Multiple above 1) with the soft edge up costs this port up to a couple of seconds a solve, so it runs in a worker and the picture keeps the previous level until the answer lands. The plugin blocks its render thread instead; the line under the picture says whenever the page is waiting.',
    'The line under the picture is not the plugin. It reports the level the plugin’s code chose and reads the B area back out of the plugin’s own SoftArea at that level — so Edge law shows an area that is not the fader and Area law shows one that is. The plugin draws no such thing.',
    'The clock is the page’s, in seconds, handed straight to the modulator’s phase. The plugin works out its host’s clock unit by watching it (Resolume sends milliseconds) and keeps time frame-relative so a six-day session does not freeze the sine; none of that is exercised here. Restart puts the phase back to zero.',
    'The About block — a text line and link buttons for the host’s panel — is absent: a web page has links of its own. Wipe has no audio path, so nothing here is missing for want of one.',
    'Nothing here is measured. The plugin’s numerical proof — hard edges on their column bitwise, soft edges integrated to 0.0000 px, every pattern’s B area within a pixel of the fader in Area law on two renderers, a circle’s edge widening as the parabola predicts — is tools/wptest in the repository, and that harness, not this page, is the reason to believe the maths.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The kit labels its clip picker "Clip". With two inputs that is ambiguous,
// so it is renamed for what it feeds, A, the layer below. Done from the page
// rather than in the kit, which is vendored into thirty repos and this is the
// first mixer; if a second arrives, that is the moment to move it upstream.
//---------------------------------------------------------------------------
// The kit builds the variant dropdown (Clip B) before its clip picker, so
// Clip A is moved in front of it too: A, then B, the way the inputs are
// numbered.
for (const label of document.querySelectorAll('.transport__label')) {
  if (label.textContent === 'Clip') {
    label.textContent = 'Clip A';
    label.title = 'A, the layer below: what the fader wipes away from.';
    const fieldA = label.closest('.transport__field');
    const fieldB = [...document.querySelectorAll('.transport__label')]
      .find((l) => l.textContent === 'Clip B')?.closest('.transport__field');
    if (fieldA && fieldB) fieldB.before(fieldA);
  }
}

// The statistics line, under the transport.
const statLine = document.createElement('p');
statLine.className = 'stage__status';
statLine.setAttribute('aria-live', 'off');
statLine.dataset.role = 'wipe-stats';
document.querySelector('.stage')?.append(statLine);
if (statLine.isConnected) {
  const tick = () => {
    if (statLine.textContent !== stats.text) statLine.textContent = stats.text;
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

redraw = mounted?.redraw ?? (() => {});

export { mounted };
