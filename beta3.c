/** beta3 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <jack/jack.h>
#include <jack/midiport.h>

typedef jack_default_audio_sample_t sample_t; // sample type is float
typedef jack_nframes_t sample_index_t;
typedef unsigned char note_index_t; // index of a note

// logging levels:
#define PRINTF_ERROR(...) fprintf(stdout, __VA_ARGS__)
#define PRINTF_INFO(...) fprintf(stdout, __VA_ARGS__)
#define PRINTF_DEBUG(...) fprintf(stdout, __VA_ARGS__)

#define OCTAVE 12 // octave interval
#define FIFTH 7 // fifth interval
#define THIRD 4 // major third interval

#define NUM_WAVES 91
#define NUM_KEYS 5 * OCTAVE + 1
#define DUMMY_NOTE NUM_KEYS
#define NUM_DRAWBARS 9

// drawbars gains:
// http://www.dairiki.org/HammondWiki/Drawbars
#define DB_G8 1
#define DB_G7 0.708
#define DB_G6 0.5
#define DB_G5 0.355
#define DB_G4 0.25
#define DB_G3 0.178
#define DB_G2 0.125
#define DB_G1 0.0891
#define DB_G0 0.0

static sample_t DB_G[9] = {DB_G0, DB_G1, DB_G2, DB_G3, DB_G4, DB_G5, DB_G6, DB_G7, DB_G8};

static note_index_t drawbar_harmonic_offset[NUM_DRAWBARS] = {-OCTAVE, FIFTH, 0, OCTAVE, OCTAVE+FIFTH, 2*OCTAVE, 2*OCTAVE+THIRD, 2*OCTAVE+FIFTH, 3*OCTAVE};

typedef struct {
	note_index_t indexx;
	sample_index_t length;
	sample_t *samples;
} wave_t;

typedef struct {
	wave_t waves[NUM_WAVES];
} tone_generator_t;

typedef struct {
	int step;
	sample_index_t ref_time;
	sample_t amplitude;
	sample_t rate;
} envelope_t;

typedef struct {
	_Bool is_on;
	envelope_t envelope;
	note_index_t next;
	note_index_t prev;
} note_info_t;

typedef struct {
	note_info_t notes[NUM_KEYS + 1];
	int drawbar_positions[NUM_DRAWBARS];
	sample_t drawbar_gains[NUM_DRAWBARS];
	sample_t (*presets)[NUM_DRAWBARS];
	sample_t *current_preset;
} manual_t;

typedef struct {
	sample_index_t sample_rate;
	sample_index_t global_time;
	tone_generator_t tone_generator;
	manual_t manuals[2];
	struct jack_s {
		jack_client_t *client;
		jack_port_t *midi_in_port;
		jack_port_t *audio_out_port;
	} jack;
		
} instrument_t;

sample_t presets[][NUM_DRAWBARS] = {
	{0, 0, DB_G8, 0, 0, 0, 0, 0, 0},
	{DB_G8, DB_G8, DB_G8, 0, 0, 0, 0, 0, 0}
};

instrument_t my; // the global state of the instrument

_Bool add_note(note_info_t notes[], note_index_t note_index) {
	if (notes[note_index].is_on) return 0;
	notes[note_index].is_on = 1;
	note_index_t first_index = notes[DUMMY_NOTE].next;
	notes[DUMMY_NOTE].next = note_index;
	notes[note_index].next = first_index;
	notes[note_index].prev = DUMMY_NOTE;
	notes[first_index].prev = note_index;
	return 1;
}

_Bool remove_note(note_info_t notes[], note_index_t note_index) {
	if (!notes[note_index].is_on) return 0;
	notes[note_index].is_on = 0;
	note_index_t next = notes[note_index].next;
	note_index_t prev = notes[note_index].prev;
	notes[prev].next = next;
	notes[next].prev = prev;
	return 1;
}

static inline void update_envelope(envelope_t *envelope) {
	sample_t amp = envelope->amplitude;
	if (amp != 1.0) {
		if (amp < DB_G1) {
			envelope->amplitude = DB_G1;
		} else {
			amp *= envelope->rate;
			if (amp > 1.0) amp = 1.0;
			envelope->amplitude = amp;
		}
	}
}

int process_callback(sample_index_t nframes, void *arg) {
	// handle MIDI in:
	void* midi_in_buf = jack_port_get_buffer(my.jack.midi_in_port, nframes);
	int midi_event_count = jack_midi_get_event_count(midi_in_buf);
	if (midi_event_count > 0) {
		jack_midi_event_t midi_event;
		for (int k = 0; k < midi_event_count; k++) {
			jack_midi_event_get(&midi_event, midi_in_buf, k);
			jack_midi_data_t code = midi_event.buffer[0];
			// PRINTF_DEBUG("$ midi in - %x\n", code);
			if (code == 0x90) {
				// note on:
				note_index_t note_index = midi_event.buffer[1] - 3 * OCTAVE;
				if (0 <= note_index && note_index < NUM_KEYS) {
					//PRINTF_DEBUG("$ midi in - note on %d\n", note_index);
					note_info_t *notes = my.manuals[0].notes;
					if (add_note(notes, note_index)) {
						notes[note_index].envelope.amplitude = 0;
						//notes[note_index].envelope.rate = 1.0 + 24*2.3f/20.0f/my.sample_rate;
						notes[note_index].envelope.rate = 1.0 + 100000/my.sample_rate;
						notes[note_index].envelope.ref_time = my.global_time;
					}
				}
			} else if (code == 0x80) {
				// note off:
				note_index_t note_index = midi_event.buffer[1] - 3 * OCTAVE;
				if (0 <= note_index && note_index < NUM_KEYS) {
					//PRINTF_DEBUG("$ midi in - note off %d\n", note_index);
					note_info_t *notes = my.manuals[0].notes;
					remove_note(notes, note_index);
				}
			} else if (code == 0xB0) {
				// cc:
				PRINTF_DEBUG("$ cc %d %d\n", midi_event.buffer[1], midi_event.buffer[2]);
				int cc_num = midi_event.buffer[1];
				int cc_val = midi_event.buffer[2];
				if (16 <= cc_num && cc_num <= 24) {
					int db_num = cc_num - 16;
					int db_pos = (cc_val + 8) >> 4;
					PRINTF_DEBUG("$ db %d %d\n", db_num, db_pos);
					my.manuals[0].drawbar_gains[db_num] = DB_G[db_pos];
				}
			}
		}
	}
	// handle audio out:
	jack_default_audio_sample_t *audio_out_buf = jack_port_get_buffer(my.jack.audio_out_port, nframes);
	for (sample_index_t k = 0; k < nframes; k++) {
		sample_t sample = 0;
		// iterate active notes:
		note_info_t *notes = my.manuals[0].notes;
		for (note_index_t note_index = notes[DUMMY_NOTE].next;
				note_index != DUMMY_NOTE;
				note_index = notes[note_index].next) {
			sample_t envelope = notes[note_index].envelope.amplitude;
			for (int d = 0; d < NUM_DRAWBARS; d++) {
				note_index_t wave_index = note_index + OCTAVE + drawbar_harmonic_offset[d];
				if (wave_index < OCTAVE) {
					wave_index += OCTAVE; // lower foldback
				} else {
					while (wave_index >= NUM_WAVES) wave_index -= OCTAVE; // upper foldback
				}
				wave_t *wave = &my.tone_generator.waves[wave_index];
				sample_t drawbar_gain = my.manuals[0].drawbar_gains[d];
				if (wave_index < NUM_WAVES) {
					sample += envelope * drawbar_gain * wave->samples[my.global_time % wave->length];
				}
			}
			update_envelope(&notes[note_index].envelope);
		}
		audio_out_buf[k] = sample;
		my.global_time++;
	}
	//TODO http://www.dairiki.org/HammondWiki/HarmonicLeakage
	return 0;      
}

void shutdown_callback(void *arg) {
	PRINTF_INFO("shutdown received, exiting\n");
	exit(1);
}

void signal_handler(int sig) {
	jack_client_close(my.jack.client);
	PRINTF_INFO("signal %d received, exiting\n", sig);
	exit(1);
}

void compute_tone_wave(wave_t *wave, int wave_index, double note_freq, double lfo_freq, wave_t *work) {
	sample_t gain = 0.025;
	for (sample_index_t k = 0; k < work->length; k++) {
		double ph = note_freq * k / my.sample_rate;
		if (ph >= 9.5 && fabs(fmod(ph, 1.0)) < 1e-4) {
			wave->length = k;
			wave->samples = (sample_t *)malloc(sizeof(sample_t) * k);
			memcpy(wave->samples, work->samples, sizeof(sample_t) * k);
			PRINTF_DEBUG("$ init note #%d %gHz %dspls\n", wave_index, note_freq, k);
			return;
		}
		sample_t amp_mod = (1 + 0.00002 * sin(2 * M_PI * lfo_freq * k / my.sample_rate));
		work->samples[k] = amp_mod * gain * (sin(2 * M_PI * ph) + 0.015 * sin(4 * M_PI * ph));
	}
	PRINTF_ERROR("uh, cannot loop note #%d %gHz ...\n", wave_index, note_freq);
	exit(1);
}

static int driving[OCTAVE] = { 85,  71,  67, 105, 103,  84,  74,  98,  96,  88,  67, 108};
static int driven [OCTAVE] = {104,  82,  73, 108, 100,  77,  64,  80,  74,  64,  46,  70};
                             // C    C#   D    D#   E    F    F#   G    G#   A    A#   B

void init_tone_generator() {
	// http://www.dairiki.org/HammondWiki/ToneWheel
	// http://www.dairiki.org/HammondWiki/GearRatio
	wave_t work;
	work.length = 10 * my.sample_rate; // room for 10secs of samples;
	work.samples = (sample_t *)malloc(sizeof(sample_t) * work.length);
	// computes 73 sine waves spanning 8 octaves; last octave is incomplete and uses divisor based on F
	double motor_freq = 20.0;
	int wave_index = 0;
	int teeth = 2;
	for (int i = 0; i < 8; i++) {
		int jmax = i < 7 ? 12 : 7;
		for (int j = 0; j < jmax; j++) {
			double freq = i < 7 
				? motor_freq * teeth * driving[j] / driven[j]
				: motor_freq * 192 * driving[j+5] / driven[j+5];
			wave_t *wave = &my.tone_generator.waves[wave_index];
			compute_tone_wave(wave, wave_index, freq, freq / teeth, &work);
			wave_index++;
		}
		teeth *= 2;
	}
	free(work.samples);
}

void init() {
	init_tone_generator();
	my.manuals[0].notes[DUMMY_NOTE].next = DUMMY_NOTE;
	my.manuals[0].notes[DUMMY_NOTE].prev = DUMMY_NOTE;
	my.manuals[0].drawbar_gains[0] = DB_G8;
	my.manuals[0].drawbar_gains[1] = DB_G8;
	my.manuals[0].drawbar_gains[2] = DB_G8;
	//my.manuals[0].drawbar_gains[8] = DB_G5;
	my.manuals[0].presets = presets;
}

int main(int argc, char *argv[]) {

	/* connect to the JACK server */
	my.jack.client = jack_client_open("beta3", JackNullOption, NULL);
	if (!my.jack.client) {
		PRINTF_ERROR("uh, jack server not running?\n");
		return 1;
	}

	/* register the callbacks: */
	jack_set_process_callback(my.jack.client, process_callback, 0);
	jack_on_shutdown(my.jack.client, shutdown_callback, 0);
	signal(SIGQUIT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGINT, signal_handler);

	/* display the current sample rate. */
	my.sample_rate = jack_get_sample_rate(my.jack.client);
	printf("sample rate: %" PRIu32 "\n", (uint32_t)my.sample_rate);
	int rt = jack_is_realtime(my.jack.client);
	printf("is realtime: %d\n", rt);

	init();
	my.global_time = 0;

	/* create two ports */
	my.jack.midi_in_port = jack_port_register(my.jack.client, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
	my.jack.audio_out_port = jack_port_register(my.jack.client, "output", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

	/* tell the JACK server that we are ready to roll */
	if (jack_activate(my.jack.client)) {
		PRINTF_ERROR("uh, cannot activate client");
		return 1;
	}

	/* connect the ports */
	const char **ports;
	if ((ports = jack_get_ports(my.jack.client, NULL, NULL, JackPortIsPhysical|JackPortIsInput)) != NULL) {
		if (jack_connect(my.jack.client, jack_port_name(my.jack.audio_out_port), ports[0])) {
			PRINTF_ERROR("uh, cannot connect audio output ports\n");
		}
		free(ports);
	}
	
 	/* run until interrupted */
	while(1) sleep(10);

}

