//  ---------------------------------------------------------------------------
//  This file is part of reSID, a MOS6581 SID emulator engine.
//  Copyright (C) 2004  Dag Lem <resid@nimrod.no>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  US
//  ---------------------------------------------------------------------------

#include "sidfp.h"
#include <math.h>
#include <cstdint>  // for uintptr_t

#define RINGSIZE 2048

#ifdef __SSE__
#include <xmmintrin.h>
#endif

/* tables used by voice/wavegen/envgen */
float dac[12];
float env_dac[256];
float wftable[11][4096];
float leftSample;
float rightSample;
float leftSamplePrevious;
float rightSamplePrevious;

void SIDFP::set_voice_nonlinearity(float nl)
{
	/* all voices, waves, etc. share the same tables */
	voice[0].envelope.set_nonlinearity(nl);
	voice[0].wave.set_nonlinearity(nl);
	voice[0].wave.rebuild_wftable();
	filter.set_nonlinearity(nl);
}

float SIDFP::kinked_dac(const int x, const float nonlinearity, const int max)
{
	float value = 0.f;

	int bit = 1;
	float weight = 1.f;
	const float dir = 2.0f * nonlinearity;
	for (int i = 0; i < max; i++) {
		if (x & bit)
			value += weight;
		bit <<= 1;
		weight *= dir;
	}

	return value / (weight / nonlinearity / nonlinearity) * (1 << max);
}

// ----------------------------------------------------------------------------
// Constructor.
// ----------------------------------------------------------------------------
SIDFP::SIDFP()
{
	// Initialize pointers.
	sampleL = 0;
	sampleR = 0;
	fir = 0;
	lastsample[0] = lastsample[1] = lastsample[2] = 0;
	filtercyclegate = 0;

	set_sampling_parameters(985248, SAMPLE_INTERPOLATE, 44100);

	bus_value = 0;
	bus_value_ttl = 0;

	input(0);
}


// ----------------------------------------------------------------------------
// Destructor.
// ----------------------------------------------------------------------------
SIDFP::~SIDFP()
{
	delete[] sampleL;
	delete[] sampleR;
	delete[] fir;
}

// ----------------------------------------------------------------------------
// Set chip model.
// ----------------------------------------------------------------------------
void SIDFP::set_chip_model(chip_model model)
{
	for (int i = 0; i < 3; i++) {
		voice[i].set_chip_model(model);
	}
	voice[0].wave.rebuild_wftable();

	filter.set_chip_model(model);
}

// ----------------------------------------------------------------------------
// SID reset.
// ----------------------------------------------------------------------------
void SIDFP::reset()
{
	for (int i = 0; i < 3; i++) {
		voice[i].reset();
	}
	filter.reset();
	extfilt.reset();

	bus_value = 0;
	bus_value_ttl = 0;
}


// ----------------------------------------------------------------------------
// Write 16-bit sample to audio input.
// NB! The caller is responsible for keeping the value within 16 bits.
// Note that to mix in an external audio signal, the signal should be
// resampled to 1MHz first to avoid sampling noise.
// ----------------------------------------------------------------------------
void SIDFP::input(int sample)
{
	// Voice outputs are 20 bits. Scale up to match three voices in order
	// to facilitate simulation of the MOS8580 "digi boost" hardware hack.
	ext_in = static_cast<float>((sample << 4) * 3);
}

// ----------------------------------------------------------------------------
// Read sample from audio output, 16-bit result.
// Do not use this directly, rather use the high-quality resampling outputs.
// ----------------------------------------------------------------------------
float SIDFP::output(float *left, float *right)
{
	/* Scale to roughly -1 .. 1 range. Voices go from -2048 to 2048 or so,
	 * envelope from 0 to 255, there are 3 voices, and there's factor of 2
	 * for resonance. */

	 // get left
//	float outLeft = extfilt.output(0) * (1.f / (2047.f * 255.f * 3.0f * 2.0f));
	// get right
//	float outRight = extfilt.output(1) * (1.f / (2047.f * 255.f * 3.0f * 2.0f));

	// get left
	float outLeft = extfilt.outputL() * (1.f / (2047.f * 255.f * 3.0f * 2.0f));
	// get right
	float outRight = extfilt.outputR() * (1.f / (2047.f * 255.f * 3.0f * 2.0f));

	*left = outLeft;
	*right = outRight;
	return outLeft;
}

// ----------------------------------------------------------------------------
// Read registers.
//
// Reading a write only register returns the last byte written to any SID
// register. The individual bits in this value start to fade down towards
// zero after a few cycles. All bits reach zero within approximately
// $2000 - $4000 cycles.
// It has been claimed that this fading happens in an orderly fashion, however
// sampling of write only registers reveals that this is not the case.
// NB! This is not correctly modeled.
// The actual use of write only registers has largely been made in the belief
// that all SID registers are readable. To support this belief the read
// would have to be done immediately after a write to the same register
// (remember that an intermediate write to another register would yield that
// value instead). With this in mind we return the last value written to
// any SID register for $2000 cycles without modeling the bit fading.
// ----------------------------------------------------------------------------
reg8 SIDFP::read(reg8 offset)
{
	switch (offset) {
	case 0x19:
		return potx.readPOT();
	case 0x1a:
		return poty.readPOT();
	case 0x1b:
		return voice[2].wave.readOSC(voice[0].wave);
	case 0x1c:
		return voice[2].envelope.readENV();
	default:
		return bus_value;
	}
}


// ----------------------------------------------------------------------------
// Write registers.
// ----------------------------------------------------------------------------
void SIDFP::write(reg8 offset, reg8 value)
{
	bus_value = value;
	bus_value_ttl = 34000;

	switch (offset) {
	case 0x00:
		voice[0].wave.writeFREQ_LO(value);
		break;
	case 0x01:
		voice[0].wave.writeFREQ_HI(value);
		break;
	case 0x02:
		voice[0].wave.writePW_LO(value);
		break;
	case 0x03:
		voice[0].wave.writePW_HI(value);
		break;
	case 0x04:
		voice[0].writeCONTROL_REG(voice[1].wave, value);
		break;
	case 0x05:
		voice[0].envelope.writeATTACK_DECAY(value);
		break;
	case 0x06:
		voice[0].envelope.writeSUSTAIN_RELEASE(value);
		break;
	case 0x07:
		voice[1].wave.writeFREQ_LO(value);
		break;
	case 0x08:
		voice[1].wave.writeFREQ_HI(value);
		break;
	case 0x09:
		voice[1].wave.writePW_LO(value);
		break;
	case 0x0a:
		voice[1].wave.writePW_HI(value);
		break;
	case 0x0b:
		voice[1].writeCONTROL_REG(voice[2].wave, value);
		break;
	case 0x0c:
		voice[1].envelope.writeATTACK_DECAY(value);
		break;
	case 0x0d:
		voice[1].envelope.writeSUSTAIN_RELEASE(value);
		break;
	case 0x0e:
		voice[2].wave.writeFREQ_LO(value);
		break;
	case 0x0f:
		voice[2].wave.writeFREQ_HI(value);
		break;
	case 0x10:
		voice[2].wave.writePW_LO(value);
		break;
	case 0x11:
		voice[2].wave.writePW_HI(value);
		break;
	case 0x12:
		voice[2].writeCONTROL_REG(voice[0].wave, value);
		break;
	case 0x13:
		voice[2].envelope.writeATTACK_DECAY(value);
		break;
	case 0x14:
		voice[2].envelope.writeSUSTAIN_RELEASE(value);
		break;
	case 0x15:
		filter.writeFC_LO(value);
		break;
	case 0x16:
		filter.writeFC_HI(value);
		break;
	case 0x17:
		filter.writeRES_FILT(value);
		break;
	case 0x18:
		filter.writeMODE_VOL(value);
		break;
	default:
		break;
	}
}


// ----------------------------------------------------------------------------
// SID voice muting.
// ----------------------------------------------------------------------------
void SIDFP::mute(reg8 channel, bool enable)
{
	// Only have 3 voices!
	if (channel >= 3)
		return;

	voice[channel].mute(enable);
}

// ----------------------------------------------------------------------------
// Enable filter.
// ----------------------------------------------------------------------------
void SIDFP::enable_filter(bool enable)
{
	filter.enable_filter(enable);
}

// ----------------------------------------------------------------------------
// I0() computes the 0th order modified Bessel function of the first kind.
// This function is originally from resample-1.5/filterkit.c by J. O. Smith.
// ----------------------------------------------------------------------------
double SIDFP::I0(double x)
{
	// Max error acceptable in I0 could be 1e-6, which gives that 96 dB already.
	// I'm overspecify these errors to get a beautiful FFT dump of the FIR.
	const double I0e = 1e-10;

	double sum, u, halfx, temp;
	int n;

	sum = u = n = 1;
	halfx = x / 2.0;

	do {
		temp = halfx / n++;
		u *= temp * temp;
		sum += u;
	} while (u >= I0e * sum);

	return sum;
}


// ----------------------------------------------------------------------------
// Setting of SID sampling parameters.
//
// Use a clock freqency of 985248Hz for PAL C64, 1022730Hz for NTSC C64.
// The default end of passband frequency is pass_freq = 0.9*sample_freq/2
// for sample frequencies up to ~ 44.1kHz, and 20kHz for higher sample
// frequencies.
//
// For resampling, the ratio between the clock frequency and the sample
// frequency is limited as follows:
//   125*clock_freq/sample_freq < 16384
// E.g. provided a clock frequency of ~ 1MHz, the sample frequency can not
// be set lower than ~ 8kHz. A lower sample frequency would make the
// resampling code overfill its 16k sample ring buffer.
//
// The end of passband frequency is also limited:
//   pass_freq <= 0.9*sample_freq/2

// E.g. for a 44.1kHz sampling rate the end of passband frequency is limited
// to slightly below 20kHz. This constraint ensures that the FIR table is
// not overfilled.
// ----------------------------------------------------------------------------
bool SIDFP::set_sampling_parameters(double clock_freq, sampling_method method,
	double sample_freq, double pass_freq)
{
	filter.set_clock_frequency(static_cast<float>(clock_freq * 0.5));
	extfilt.set_clock_frequency(static_cast<float>(clock_freq * 0.5));

	cycles_per_sample = static_cast<float>(clock_freq / sample_freq);

	// FIR initialization is only necessary for resampling.
	if (method != SAMPLE_RESAMPLE_INTERPOLATE)
	{
		sampling = method;

		delete[] sampleL;
		delete[] sampleR;
		delete[] fir;
		sampleL = 0;
		sampleR = 0;
		fir = 0;
		sample_prevL = 0;
		sample_prevR = 0;
		return true;
	}

	sample_offset = 0;

	// Allocate sample buffer.
	// JP TrueStereo = *2
	if (!sampleL)
	{
		sampleL = new float[RINGSIZE * 2];
		sampleR = new float[RINGSIZE * 2];
	}

	// Clear sample buffer.
	for (int j = 0; j < RINGSIZE * 2; j++)
	{
		sampleL[j] = 0;
		sampleR[j] = 0;
	}
	sample_index = 0;

	/* Up to 20 kHz or at most 90 % of passband if it's lower than 20 kHz. */
	if (pass_freq > 20000)
		pass_freq = 20000;
	if (2 * pass_freq / sample_freq > 0.9)
		pass_freq = 0.9*sample_freq / 2;

	// 16 bits -> -96dB stopband attenuation.
	const double A = -20 * log10(1.0 / (1 << 16));

	// For calculation of beta and N see the reference for the kaiserord
	// function in the MATLAB Signal Processing Toolbox:
	// http://www.mathworks.com/access/helpdesk/help/toolbox/signal/kaiserord.html
	const double beta = 0.1102*(A - 8.7);
	const double I0beta = I0(beta);

	// Since we clock the filter at half the rate, we need to design the FIR
	// with the reduced rate in mind.
	double f_cycles_per_sample = (clock_freq * 0.5) / sample_freq;
	double f_samples_per_cycle = sample_freq / (clock_freq * 0.5);

	double aliasing_allowance = sample_freq / 2 - 20000;
	// no allowance to 20 kHz
	if (aliasing_allowance < 0)
		aliasing_allowance = 0;

	double transition_bandwidth = sample_freq / 2 - pass_freq + aliasing_allowance;
	{
		// The filter order will maximally be 124 with the current constraints.
		// N >= (96.33 - 7.95)/(2 * pi * 2.285 * (maxfreq - passbandfreq) >= 123
		// The filter order is equal to the number of zero crossings, i.e.
		// it should be an even number (sinc is symmetric about x = 0).
		//
		// XXX: analysis indicates that the filter is slighly overspecified by
		//      there constraints. Need to check why. One possibility is the
		//      level of audio being in truth closer to 15-bit than 16-bit.
		int N = static_cast<int>((A - 7.95) / (2 * M_PI * 2.285 * transition_bandwidth / sample_freq) + 0.5);
		N += N & 1;

		// The filter length is equal to the filter order + 1.
		// The filter length must be an odd number (sinc is symmetric about x = 0).
		fir_N = static_cast<int>(N*f_cycles_per_sample) + 1;
		fir_N |= 1;

		// Check whether the sample ring buffer would overfill.
		if (fir_N > RINGSIZE - 1)
			return false;

		/* Error is bound by 1.234 / L^2, so for 16-bit: sqrt(1.234 * (1 << 16)) */
		fir_RES = static_cast<int>(sqrt(1.234 * (1 << 16)) / f_cycles_per_sample + 0.5);
	}
	sampling = method;

	// Allocate memory for FIR tables.
	delete[] fir;
	fir = new float[fir_N*fir_RES];

	// The cutoff frequency is midway through the transition band
	double wc = (pass_freq + transition_bandwidth / 2) / sample_freq * M_PI * 2;

	// Calculate fir_RES FIR tables for linear interpolation.
	for (int i = 0; i < fir_RES; i++) {
		double j_offset = double(i) / fir_RES;
		// Calculate FIR table. This is the sinc function, weighted by the
		// Kaiser window.
		for (int j = 0; j < fir_N; j++) {
			double jx = double(j) - fir_N / 2.f - j_offset;
			double wt = wc * jx / f_cycles_per_sample;
			double temp = jx / (fir_N / 2);
			double Kaiser =
				fabs(temp) <= 1 ? I0(beta*sqrt(1 - temp * temp)) / I0beta : 0;
			// between 1e-7 and 1e-8 the FP result approximates to 1 due to FP limits
			double sincwt =
				fabs(wt) >= 1e-8 ? sin(wt) / wt : 1;
			fir[i * fir_N + j] = static_cast<float>(f_samples_per_cycle*wc / M_PI * sincwt*Kaiser);
		}
	}

	return true;
}

inline
void SIDFP::age_bus_value(cycle_count n) {
	// Age bus value. This is not supposed to be cycle exact,
	// so it should be safe to approximate.
	if (bus_value_ttl != 0) {
		bus_value_ttl -= n;
		if (bus_value_ttl <= 0) {
			bus_value = 0;
			bus_value_ttl = 0;
		}
	}
}

float testPan = 0;
// ----------------------------------------------------------------------------
// SID clocking - 1 cycle.
// ----------------------------------------------------------------------------
void SIDFP::clock()
{
	// Clock amplitude modulators.
	for (int i = 0; i < 3; i++) {
		voice[i].envelope.clock();
		voice[i].wave.clock();
	}

	voice[0].wave.synchronize(voice[1].wave, voice[2].wave);
	voice[1].wave.synchronize(voice[2].wave, voice[0].wave);
	voice[2].wave.synchronize(voice[0].wave, voice[1].wave);

	/* because the analog parts are relatively expensive and do not really need
	 * the precision of 1 MHz calculations, I average successive samples here to
	 * reduce the cpu drain for filter calculations and output resampling. */
	float voicestate[3];
	float voiceLR[6];
	voicestate[0] = voice[0].output(voice[2].wave);
	voicestate[1] = voice[1].output(voice[0].wave);
	voicestate[2] = voice[2].output(voice[1].wave);

	for (int i = 0;i < 3;i++)
	{
		int k = i + 3;
		int pan = voice[i].getPan();
		if (pan == 0xf)	//0-6 = left pan. 8-e = right pan. 7 = center (f not used)
			pan--;
		pan %= 15;
		float fpan = (float)pan;
		fpan /= 14.0f;
		voiceLR[i] = voicestate[i] * (1 - fpan);
		voiceLR[k] = voicestate[i] * (fpan);
	}


	// JP: voiceState = mono output
	// 1. Handling panning for each voice - convert voicestate[n] to two buffers VoiceL[n] and Voice R[n]
	// 2. change lastsample[n] to be lastsampleL[n] and lastsampleR[n]
	// 3. Modify filterfp.h AND extfiltfp.h so that Vlp, Vhp and Vbp are stereo arrays
	// 4. call the following twice
	// 5. Modify all clock routines below that call .output() to output to two buffers (L/R) and handle procssing / mixing as such
	// 6. (start with just modifying one clock function.. And just use left output if mono is selected somehow...)

	/* for every second sample in sequence, clock filter */
	if (filtercyclegate++ & 1)
	{

		float f = 0;

		for (int i = 0;i < 2;i++)
		{
			int k = i * 3;
			f = filter.clock(
				(lastsample[0 + k] + voiceLR[0 + k]) * 0.5f,
				(lastsample[1 + k] + voiceLR[1 + k]) * 0.5f,
				(lastsample[2 + k] + voiceLR[2 + k]) * 0.5f,
				ext_in, i);

			extfilt.clock(f, i);

		}
	}

	for (int i = 0; i < 2; i++)
	{
		int k = i * 3;
		lastsample[0 + k] = voiceLR[0 + k];
		lastsample[1 + k] = voiceLR[1 + k];
		lastsample[2 + k] = voiceLR[2 + k];
	}

}

// ----------------------------------------------------------------------------
// SID clocking with audio sampling.
//
// The example below shows how to clock the SID a specified amount of cycles
// while producing audio output:
//
// while (delta_t) {
//   bufindex += sid.clock(delta_t, buf + bufindex, buflength - bufindex);
//   write(dsp, buf, bufindex*2);
//   bufindex = 0;
// }
//
// ----------------------------------------------------------------------------
int SIDFP::clock(cycle_count& delta_t, short* buf, int n, int bufferHalfSize, int interleave)
{

	/* XXX I assume n is generally large enough for delta_t here... */
	age_bus_value(delta_t);

	/* We can only control that SSE is really used for GCC */
#if defined(__SSE__) && defined(__GNUC__)
	int old = _mm_getcsr();
	_mm_setcsr(old | _MM_FLUSH_ZERO_ON);
#endif
	int res;
	switch (sampling) {
	default:
	case SAMPLE_INTERPOLATE:
		res = clock_interpolate(delta_t, buf, n, bufferHalfSize, interleave);
		break;
	case SAMPLE_RESAMPLE_INTERPOLATE:
		res = clock_resample_interpolate(delta_t, buf, n, bufferHalfSize, interleave);
		break;
	}
#if defined(__SSE__) && defined(__GNUC__)
	_mm_setcsr(old);
#else
	filter.nuke_denormals();
	extfilt.nuke_denormals();
#endif

	return res;
}

int SIDFP::clock_fast(cycle_count& delta_t, short* buf, int n, int bufferHalfSize, int interleave)
{
	age_bus_value(delta_t);
#if defined(__SSE__) && defined(__GNUC__)
	int old = _mm_getcsr();
	_mm_setcsr(old | _MM_FLUSH_ZERO_ON);
#endif
	int res = clock_interpolate(delta_t, buf, n, bufferHalfSize, interleave);
#if defined(__SSE__) && defined(__GNUC__)
	_mm_setcsr(old);
#else
	filter.nuke_denormals();
	extfilt.nuke_denormals();
#endif
	return res;
}


// ----------------------------------------------------------------------------
// SID clocking with audio sampling - cycle based with linear sample
// interpolation.
//
// Here the chip is clocked every cycle. This yields higher quality
// sound since the samples are linearly interpolated, and since the
// external filter attenuates frequencies above 16kHz, thus reducing
// sampling noise.
// ----------------------------------------------------------------------------
inline
int SIDFP::clock_interpolate(cycle_count& delta_t, short* buf, int n, int bufferHalfSize,
	int interleave)
{
	int s = 0;
	int i;


	for (;;) {
		float next_sample_offset = sample_offset + cycles_per_sample;
		int delta_t_sample = static_cast<int>(next_sample_offset);
		if (delta_t_sample > delta_t) {
			break;
		}
		if (s >= n) {
			return s;
		}
		for (i = 0; i < delta_t_sample - 1; i++) {
			clock();
		}
		if (i < delta_t_sample) {
			//		sample_prev = output(&leftSamplePrevious, &rightSamplePrevious);

			output(&sample_prevL, &sample_prevR);
			clock();
		}

		delta_t -= delta_t_sample;
		sample_offset = next_sample_offset - delta_t_sample;

		//float sample_now = output(&leftSample, &rightSample);
		output(&leftSample, &rightSample);


		// JP
		// Need to do this bit in a loop for Left/Right (using leftSample & leftSamplePrevious, etc..)
		// Need to either double buf[] size, or have 2 buffers for left/right
		// Need to check what format we eventually handle stereo in, so we can make this the same

		int sample_int = (int)((sample_prevL + (sample_offset * (leftSample - sample_prevL))) * 32768.0f);
		if (sample_int < -32768) sample_int = -32768;
		if (sample_int > 32767) sample_int = 32767;
		buf[s * interleave] = sample_int;

		sample_int = (int)((sample_prevR + (sample_offset * (rightSample - sample_prevR))) * 32768.0f);
		if (sample_int < -32768) sample_int = -32768;
		if (sample_int > 32767) sample_int = 32767;
		buf[bufferHalfSize + (s * interleave)] = sample_int;
		s++;

		sample_prevL = leftSample;
		sample_prevR = rightSample;
	}

	for (i = 0; i < delta_t - 1; i++) {
		clock();
	}
	if (i < delta_t) {
		//	sample_prev = output(&leftSample, &rightSample);

		output(&sample_prevL, &sample_prevR);
		clock();
	}
	sample_offset -= delta_t;
	delta_t = 0;
	return s;
}

static float convolve(const float *a, const float *b, int n)
{
	float out = 0.f;
#ifdef __SSE__

	__m128 out4 = { 0, 0, 0, 0 };

	/* examine if we can use aligned loads on both pointers -- some x86-32/64
	 * hackery here ... should use uintptr_t, but that needs --std=C99... */
	int diff = static_cast<int>(a - b) & 0xf;
	int a_align = static_cast<int>(reinterpret_cast<uintptr_t>(a)) & 0xf;

	/* advance if necessary. We can't let n fall < 0, so no while (n --). */
	while (n > 0 && a_align != 0 && a_align != 16) {
		out += (*(a++)) * (*(b++));
		n--;
		a_align += 4;
	}

	if (diff == 0) {
		while (n >= 4) {
			out4 = _mm_add_ps(out4, _mm_mul_ps(_mm_load_ps(a), _mm_load_ps(b)));
			a += 4;
			b += 4;
			n -= 4;
		}
	}
	else {
		/* loadu is difficult to optimize:
		 *
		 * - using load and movhl tricks to sync the halves was not noticeably
		 *   faster, less than 1 % and that might have been measurement error.
		 * - preparing copies of b for syncing with any alignment increased
		 *   memory pressure to the point that cache misses made it slower!
		 */
		while (n >= 4) {
			out4 = _mm_add_ps(out4, _mm_mul_ps(_mm_load_ps(a), _mm_loadu_ps(b)));
			a += 4;
			b += 4;
			n -= 4;
		}
	}

	/* sum the upper half of values with the lower half, pairwise */
	out4 = _mm_add_ps(_mm_movehl_ps(out4, out4), out4);
	/* sum the value at slot 1 with the value at slot 0 */
	out4 = _mm_add_ss(_mm_shuffle_ps(out4, out4, 1), out4);
	float out_tmp;
	/* save slot 0 to out_tmp, which is now 0+1+2+3 */
	_mm_store_ss(&out_tmp, out4);
	out += out_tmp;
#endif
	while (n--)
		out += (*(a++)) * (*(b++));

	return out;
}


// ----------------------------------------------------------------------------
// SID clocking with audio sampling - cycle based with audio resampling.
//
// This is the theoretically correct (and computationally intensive) audio
// sample generation. The samples are generated by resampling to the specified
// sampling frequency. The work rate is inversely proportional to the
// percentage of the bandwidth allocated to the filter transition band.
//
// This implementation is based on the paper "A Flexible Sampling-Rate
// Conversion Method", by J. O. Smith and P. Gosset, or rather on the
// expanded tutorial on the "Digital Audio Resampling Home Page":
// http://www-ccrma.stanford.edu/~jos/resample/
//
// By building shifted FIR tables with samples according to the
// sampling frequency, this implementation dramatically reduces the
// computational effort in the filter convolutions, without any loss
// of accuracy. The filter convolutions are also vectorizable on
// current hardware.
//
// Further possible optimizations are:
// * An equiripple filter design could yield a lower filter order, see
//   http://www.mwrf.com/Articles/ArticleID/7229/7229.html
// * The Convolution Theorem could be used to bring the complexity of
//   convolution down from O(n*n) to O(n*log(n)) using the Fast Fourier
//   Transform, see http://en.wikipedia.org/wiki/Convolution_theorem
// * Simply resampling in two steps can also yield computational
//   savings, since the transition band will be wider in the first step
//   and the required filter order is thus lower in this step.
//   Laurent Ganier has found the optimal intermediate sampling frequency
//   to be (via derivation of sum of two steps):
//     2 * pass_freq + sqrt [ 2 * pass_freq * orig_sample_freq
//       * (dest_sample_freq - 2 * pass_freq) / dest_sample_freq ]
//
// NB! the result of right shifting negative numbers is really
// implementation dependent in the C++ standard.
// ----------------------------------------------------------------------------

// JP: For TrueStereo, bufferSize = n*2 (Left=0>n. Right = n>n*2)
inline
int SIDFP::clock_resample_interpolate(cycle_count& delta_t, short* buf, int n, int bufferHalfSize,
	int interleave)
{
	int s = 0;

	float *SL;
	float *SR;
	float *SL_RS;
	float *SR_RS;

	SL = &sampleL[sample_index];
	SL_RS = &sampleL[sample_index + RINGSIZE];
	SR = &sampleR[sample_index];
	SR_RS = &sampleR[sample_index + RINGSIZE];

	for (;;) {
		float next_sample_offset = sample_offset + cycles_per_sample;
		/* full clocks left to next sample */
		int delta_t_sample = static_cast<int>(next_sample_offset);
		if (delta_t_sample > delta_t || s >= n)
			break;

//		SL_RS = &sampleL[sampleIndex + RINGSIZE];
//		SR_RS = &sampleR[sampleIndex + RINGSIZE];

		//JPJP
		/* clock forward delta_t_sample samples */
		for (int i = 0; i < delta_t_sample; i++) {
			clock();
			if (filtercyclegate & 1) {

				//				sample[sample_index] = sample[sample_index + RINGSIZE] = output(&leftSample, &rightSample);
				output(&leftSample, &rightSample);

				// JP setting rightSample to 0 here still causes noise on output..

//				sampleL[sample_index] = leftSample;
//				sampleL[sample_index + RINGSIZE] = leftSample;
//				sampleR[sample_index] = rightSample;
//				sampleR[sample_index + RINGSIZE] = rightSample;

				*SL = leftSample;
				*SL_RS = leftSample;
				*SR= rightSample;
				*SR_RS= rightSample;

				SL++;
				SR++;
				SL_RS++;
				SR_RS++;

				++sample_index;
				sample_index &= RINGSIZE - 1;

				if (sample_index == 0)
				{
					SL = &sampleL[0];
					SR = &sampleR[0];
					SL_RS = &sampleL[RINGSIZE];
					SR_RS = &sampleR[RINGSIZE];
				}


			}
		}
		delta_t -= delta_t_sample;

		/* Phase of the sample in terms of clock, [0 .. 1[. */
		sample_offset = next_sample_offset - static_cast<float>(delta_t_sample);

		/* find the first of the nearest fir tables close to the phase */
		float fir_offset_rmd = sample_offset * fir_RES;
		int fir_offset = static_cast<int>(fir_offset_rmd);
		/* [0 .. 1[ */
		fir_offset_rmd -= static_cast<float>(fir_offset);

		/* find fir_N most recent samples, plus one extra in case the FIR wraps. */
		float* sample_startL = sampleL + sample_index - fir_N + RINGSIZE - 1;
		float* sample_startR = sampleR + sample_index - fir_N + RINGSIZE - 1;

		float v1L = convolve(sample_startL, fir + fir_offset * fir_N, fir_N);
		float v1R = convolve(sample_startR, fir + fir_offset * fir_N, fir_N);

		// Use next FIR table, wrap around to first FIR table using
		// the next sample.
		if (++fir_offset == fir_RES) {
			fir_offset = 0;
			++sample_startL;
			++sample_startR;
		}
		float v2L = convolve(sample_startL, fir + fir_offset * fir_N, fir_N);
		float v2R = convolve(sample_startR, fir + fir_offset * fir_N, fir_N);

		// Linear interpolation between the sinc tables yields good approximation
		// for the exact value.

		int sample_intL = (int)((v1L + fir_offset_rmd * (v2L - v1L)) * 32768.0f);
		if (sample_intL < -32768) sample_intL = -32768;
		if (sample_intL > 32767) sample_intL = 32767;
		buf[s * interleave] = sample_intL;

		int sample_intR = (int)((v1R + fir_offset_rmd * (v2R - v1R)) * 32768.0f);
		if (sample_intR < -32768) sample_intR = -32768;
		if (sample_intR > 32767) sample_intR = 32767;
		buf[bufferHalfSize + (s * interleave)] = sample_intR;	// was using n .still crackles if i set to 0
		s++;
	}

	/* clock forward delta_t samples */
	for (int i = 0; i < delta_t; i++) {
		clock();
		if (filtercyclegate & 1) {
			//		sample[sample_index] = sample[sample_index + RINGSIZE] = output(&leftSample, &rightSample);
			output(&leftSample, &rightSample);

//			sampleL[sample_index] = leftSample;
//			sampleL[sample_index + RINGSIZE] = leftSample;
//			sampleR[sample_index] = rightSample;
//			sampleR[sample_index + RINGSIZE] = rightSample;

//			++sample_index;
//			sample_index &= RINGSIZE - 1;

			*SL = leftSample;
			*SL_RS = leftSample;
			*SR = rightSample;
			*SR_RS = rightSample;

			SL++;
			SR++;
			SL_RS++;
			SR_RS++;

			++sample_index;
			sample_index &= RINGSIZE - 1;

			if (sample_index == 0)
			{
				SL = &sampleL[0];
				SR = &sampleR[0];
				SL_RS = &sampleL[RINGSIZE];
				SR_RS = &sampleR[RINGSIZE];
			}

		}
	}
	sample_offset -= static_cast<float>(delta_t);
	delta_t = 0;
	return s;
}
