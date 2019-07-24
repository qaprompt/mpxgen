/*

    PiFmAdv - Advanced FM transmitter for the Raspberry Pi
    Copyright (C) 2017 Miegl

    See https://github.com/Miegl/PiFmAdv
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <samplerate.h>
#include <ao/ao.h>

#include "rds.h"
#include "fm_mpx.h"
#include "control_pipe.h"

#define DATA_SIZE		4096
#define OUTPUT_DATA_SIZE	8192

ao_device *device;
SRC_STATE *src_state;

static void terminate(int num)
{
    fm_mpx_close();
    close_control_pipe();
    ao_close(device);
    ao_shutdown();
    src_delete(src_state);

    printf("MPX generator stopped\n");

    exit(num);
}

static void fatal(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

void postprocess(float *inbuf, short *outbuf, size_t inbufsize, float volume) {
	int j = 0;

	for (int i = 0; i < inbufsize; i++) {
		// scale samples
		inbuf[i] /= 10;
		inbuf[i] *= 32767;
		// volume control
		inbuf[i] *= (volume / 100);
		// copy the mono channel to the two stereo channels
		outbuf[j] = outbuf[j+1] = inbuf[i];
		j += 2;
	}
}

int generate_mpx(char *audio_file, int rds, uint16_t pi, char *ps, char *rt, int *af_array, int preemphasis_cutoff, float mpx, char *control_pipe, int pty, int tp, int wait) {
	// Catch only important signals
	for (int i = 0; i < 25; i++) {
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = terminate;
		sigaction(i, &sa, NULL);
	}

	// Data structures for baseband data
	float mpx_data[DATA_SIZE];
	float rds_data[DATA_SIZE];
	float resample_out[DATA_SIZE];
	short dev_out[OUTPUT_DATA_SIZE];

	// AO
	ao_sample_format format;
	ao_initialize();
	int default_driver = ao_default_driver_id();
	memset(&format, 0, sizeof(format));
	format.bits = 16;
	format.channels = 2;
	format.rate = 192000;
	format.byte_format = AO_FMT_LITTLE;

	if ((device = ao_open_live(default_driver, &format, NULL)) == NULL) {
		fprintf(stderr, "Error: cannot open sound device.\n");
		return 1;
	}

	// SRC
	int src_error;
	size_t generated_frames;

	SRC_STATE *src_state;
	SRC_DATA src_data;

	if ((src_state = src_new(SRC_SINC_FASTEST, 1, &src_error)) == NULL) {
		fprintf(stderr, "Error: src_new failed: %s\n", src_strerror(src_error));
		return 1;
	}

	src_data.src_ratio = 192000. / 228000;
	src_data.output_frames = OUTPUT_DATA_SIZE;

	printf("Starting MPX generator\n");

	// Initialize the baseband generator
	if(fm_mpx_open(audio_file, DATA_SIZE, preemphasis_cutoff) < 0) return 1;

	// Initialize the RDS modulator
	set_rds_pi(pi);
	set_rds_ps(ps);
	set_rds_rt(rt);
	set_rds_pty(pty);
	set_rds_tp(tp);
	set_rds_ms(1);
	set_rds_ab(0);

	printf("RDS Options:\n");

	if(rds) {
		printf("RDS: %i, PI: %04X, PS: \"%s\", PTY: %i\n", rds, pi, ps, pty);
		printf("RT: \"%s\"\n", rt);
		if(af_array[0]) {
			set_rds_af(af_array);
			printf("AF: ");
			int f;
			for(f = 1; f < af_array[0]+1; f++) {
				printf("%f Mhz ", (float)(af_array[f]+875)/10);
			}
			printf("\n");
		}
	}
	else {
		printf("RDS: %i\n", rds);
	}

	// Initialize the control pipe reader
	if(control_pipe) {
		if(open_control_pipe(control_pipe) == 0) {
			printf("Reading control commands on %s.\n", control_pipe);
		} else {
			printf("Failed to open control pipe: %s.\n", control_pipe);
			control_pipe = NULL;
		}
	}

	for (;;) {
		if(control_pipe) poll_control_pipe();

		if (fm_mpx_get_samples(mpx_data, rds_data, rds, wait) < 0) break;
		src_data.input_frames = DATA_SIZE;
		src_data.data_in = mpx_data;
		src_data.data_out = resample_out;

		if ((src_error = src_process(src_state, &src_data))) {
			fprintf(stderr, "Error: src_process failed: %s\n", src_strerror(src_error));
			break;
		}

		generated_frames = src_data.output_frames_gen;
		postprocess(resample_out, dev_out, generated_frames, mpx);
		// num_bytes = generated_frames * channels * bytes per sample
		if (!ao_play(device, (char *)dev_out, generated_frames * 4)) {
			fprintf(stderr, "Error: could not play audio.\n");
			break;
		}
	}

	return 0;
}

int main(int argc, char **argv) {
	int opt;

	char *audio_file = NULL;
	char *control_pipe = NULL;
	int rds = 1;
	int alternative_freq[100] = {};
	int af_size = 0;
	char *ps = "mpxgen";
	char *rt = "mpxgen: FM Stereo and RDS encoder";
	uint16_t pi = 0x1234;
	int preemphasis_cutoff = 0;
	int pty = 0;
	int tp = 0;
	float mpx = 10;
	int wait = 0;

	const char	*short_opt = "a:P:m:W:R:i:s:r:p:T:A:C:h";
	struct option	long_opt[] =
	{
		{"audio", 	required_argument, NULL, 'a'},
		{"preemph",	required_argument, NULL, 'P'},
		{"mpx",		required_argument, NULL, 'm'},
		{"wait",	required_argument, NULL, 'W'},

		{"rds", 	required_argument, NULL, 'R'},
		{"pi",		required_argument, NULL, 'i'},
		{"ps",		required_argument, NULL, 's'},
		{"rt",		required_argument, NULL, 'r'},
		{"pty",		required_argument, NULL, 'p'},
		{"tp",		required_argument, NULL, 'T'},
		{"af",		required_argument, NULL, 'A'},
		{"ctl",		required_argument, NULL, 'C'},

		{"help",	no_argument, NULL, 'h'},
		{ 0,		0,		0,	0 }
	};

	while((opt = getopt_long(argc, argv, short_opt, long_opt, NULL)) != -1)
	{
		switch(opt)
		{
			case -1:
			case 0:
			break;

			case 'a': //audio
				audio_file = optarg;
				break;

			case 'P': //preemph
				if(strcmp("eu", optarg)==0) {
					preemphasis_cutoff = 3185;
				} else if(strcmp("us", optarg)==0) {
					preemphasis_cutoff = 2120;
				}
				else {
					preemphasis_cutoff = atoi(optarg);
				}
				break;

			case 'm': //mpx
				mpx = atoi(optarg);
				break;

			case 'W': //wait
				wait = atoi(optarg);
				break;

			case 'R': //rds
				rds = atoi(optarg);
				break;

			case 'i': //pi
				pi = (uint16_t) strtol(optarg, NULL, 16);
				break;

			case 's': //ps
				ps = optarg;
				break;

			case 'r': //rt
				rt = optarg;
				break;

			case 'p': //pty
				pty = atoi(optarg);
				break;

			case 'T': //tp
				tp = atoi(optarg);
				break;

			case 'A': //af
				af_size++;
				alternative_freq[af_size] = (int)(10*atof(optarg))-875;
				if(alternative_freq[af_size] < 1 || alternative_freq[af_size] > 204)
					fatal("Alternative Frequency has to be set in range of 87.6 Mhz - 107.9 Mhz\n");
				break;

			case 'C': //ctl
				control_pipe = optarg;
				break;

			case 'h': //help
				fatal("Help: %s\n"
				      "	[--audio (-a) file] [--mpx (-m) mpx-power]\n"
				      "	[--preemph (-P) preemphasis] [--wait (-W) wait-switch]\n"
				      "	[--rds rds-switch] [--pi pi-code] [--ps ps-text]\n"
				      "	[--rt radiotext] [--tp traffic-program] [--pty program-type]\n"
				      "	[--af alternative-freq] [--ctl (-C) control-pipe]\n", argv[0]);
				break;

			case ':':
				fatal("%s: option '-%c' requires an argument\n", argv[0], optopt);
				break;

			case '?':
			default:
				fatal("%s: option '-%c' is invalid. See -h (--help)\n", argv[0], optopt);
				break;
		}
	}

	alternative_freq[0] = af_size;

	int errcode = generate_mpx(audio_file, rds, pi, ps, rt, alternative_freq, preemphasis_cutoff, mpx, control_pipe, pty, tp, wait);

	terminate(errcode);
}
