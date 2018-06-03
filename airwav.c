/*
 *  Copyright (c) 2017 Thierry Leconte (f4dwv)
 *
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include <math.h>
#include <complex.h>
#include "filter.h"
#include "airwav.h"
#include "mp3out.h"

int verbose = 0;
int freq = 0;
int fmdemod = 0;
char *stid =  "airwav" ;
char *directory = NULL;
int gain = 1000;

#if (WITH_RTL)
int initRtl(int dev_index, int fr);
int runRtlSample(void);
int devid = 0;
int ppm = 0;
static float threshold = 1e-7;
#endif
#ifdef WITH_AIRSPY
int init_airspy(int freq);
int runAirspy(void);
static float threshold = 1e-8;
#endif

static int interval=0;
static int silent=(5*60);

static void sighandler(int signum);

static void usage(void)
{
	fprintf(stderr,
		"airwav  narrow band AM/FM streamer/recorder Copyright (c) 2018 Thierry Leconte \n\n");
	fprintf(stderr,
		"Usage: airwav [-f ][-g gain] [-t threshold ] [-l interval ] [-v] [-s stationid] [-d directoty] [-r device] frequency (in Mhz\n");
	fprintf(stderr, "\n\n");
	fprintf(stderr, " -v :\t\t\t\tverbose\n");
	fprintf(stderr, " -f :\t\t\tFM demodulation (default AM)\n");
	fprintf(stderr, " -g gain :\t\t\tgain in tenth of db (ie : 500 = 50 db)\n");
	fprintf(stderr, " -t threshold:\t\t\tsquelch thresold in db (ie : -t -70)\n");
	fprintf(stderr, " -l interval :\t\t\tmax duration of mp3 file in second (0=no limit)\n");
	fprintf(stderr, " -d dir :\t\t\tstore in mp3 files in directoy dir instead of streaming to stdout\n");
	fprintf(stderr, " -s name :\t\t\tmp3 file prefix name (default : airwav)\n");
#if WITH_RTL
	fprintf(stderr, " -p ppm :\t\t\tppm freq shift\n");
	fprintf(stderr, " -r n :\t\t\trtl device number\n");
	fprintf(stderr, "\n Example: airwav -s LFRN -d . -r 0 136.4 ");
#endif
#if WITH_AIR
	fprintf(stderr, "\n Example: airwav -s LFRN -d . 136.4 ");
#endif
	exit(1);
}

int main(int argc, char **argv)
{
	int i, c;
	struct sigaction sigact;

	while ((c = getopt(argc, argv, "vr:g:p:t:s:d:l:f")) != EOF) {
		switch ((char)c) {
		case 'v':
			verbose = 1;
			break;
		case 'f':
			fmdemod = 1;
			break;
		case 't':
			threshold = powf(10, atoi(optarg) / 10);
			break;
		case 'l':
			interval = atoi(optarg);
			break;
		case 's':
			stid = optarg;
			break;
		case 'd':
			directory = optarg;
			break;
		case 'g':
			gain = atoi(optarg);
			break;
#ifdef WITH_RTL
		case 'p':
			ppm = atoi(optarg);
			break;
		case 'r':
			devid = atoi(optarg);
			break;
#endif
		default:
			usage();
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "need frequency\n");
		exit(-2);
	}
	freq = (int)(atof(argv[optind]) * 1000000.0);

	if(freq<24000000 || freq > 1100000000) {
		fprintf(stderr, "invalid frequency\n");
		exit(-2);
	}

	if(directory==NULL)
		 interval=0;

	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);

#if (WITH_RTL)
	if (initRtl(devid, freq))
		exit(-1);
	runRtlSample();
#endif

#ifdef WITH_AIRSPY
	if (init_airspy(freq))
		exit(-1);
	runAirspy();
#endif
	sighandler(0);
	exit(0);

}

static double iirhigh(double in)
{
	static double xv[3];
	static double yv[3];
	const double HIB1 = 1.7786317778;
	const double HIB2 = -0.8008026467;

	xv[0] = xv[1];
	xv[1] = xv[2];
	xv[2] = in;
	yv[0] = yv[1];
	yv[1] = yv[2];
	yv[2] = (xv[0] + xv[2]) - 2 * xv[1] + (HIB2 * yv[0]) + (HIB1 * yv[1]);
	return yv[2];
}

#define Tc 4.0
#define Kca 1.0/1e-3/OUTFREQ
#define Kcr 1.0/500e-3/OUTFREQ

static float env;
static int ind = 0, nzind = 0;
static short outbuff[5 * 60 * OUTFREQ + 2 * 1152];
static void audioout(float V)
{
	static int zv = 0;
	static int intvcount = 0;

	if(interval && mp3fd) {
		intvcount++;

		if(intvcount> 8000 * interval) {
			mp3_encode(outbuff, ind);
			ind = 0;
			mp3_close();
		}
	}

	if (V != 0) {
		float A, lv, g;

		/* high pass */
		A = iirhigh(V);

		/* compressor */
		lv = fabsf(A);
		if (lv > env)
			/* attack */
			env = env + Kca * (lv - env);
		else
			/* release */
			env = env + Kcr * (lv - env);

		g = powf(env, 1.0 / Tc - 1.0);

		outbuff[ind++] = (short)(32768.0 * A * g);
		nzind = ind;

		zv = 0;
	} else {
		if (mp3fd == NULL)
			return;

		outbuff[ind++] = 0;

		if(directory) {
			zv++;
			if (zv > silent * OUTFREQ) {
				mp3_encode(outbuff, nzind+OUTFREQ/2);
				mp3_close();
				zv = 0;
				return;
			}
			return;
		}
	}

	if (mp3fd == NULL) {
		mp3_init();
		intvcount=0;
	}

	if (ind >= 1152) {
		mp3_encode(outbuff, ind);
		ind = 0;
	}
}

static void sighandler(int signum)
{
	if (mp3fd) {
		mp3_encode(outbuff, nzind);
		mp3_close();
	}
	exit(0);
}

#define Kc 1.0/100e-3/OUTFREQ
#define IMPLEN 1500

static void squelch(float V,float lv)
{
	static float impbuff[IMPLEN];
	static int imp = 0;
	static int gate = 0;
	static float carrier = 0;
	static int lvcpt=0;

	carrier = carrier + Kc * (lv - carrier);

	if (verbose && lvcpt==8000) {
		fprintf(stderr, "%4.1f %d\r", 10.0 * log10f(carrier),
			carrier > threshold);
		lvcpt=0;
	} else
		lvcpt++;


	if (carrier > threshold) {
		if (gate) {
			if (imp > 0 && imp < IMPLEN) {
				int i;
				env = sqrtf(carrier);
				for (i = 0; i < imp; i++)
					audioout(impbuff[i]);
				imp = 0;
			}
			audioout(V);
		} else {
			if (imp < IMPLEN) {
				impbuff[imp++] = V;
				return;
			}
			if (imp == IMPLEN) {
				int i;
				for (i = 0; i < IMPLEN; i++)
					audioout(impbuff[i]);
				audioout(V);
				gate = 1;
				imp = 0;
				return;
			}
		}
	} else {
		if (!gate) {
			if (imp > 0 && imp < IMPLEN) {
				int i;
				for (i = 0; i < imp; i++)
					audioout(0);
				imp = 0;
			}
			audioout(0);
		} else {
			if (imp < IMPLEN) {
				impbuff[imp++] = V;
				return;
			}
			if (imp == IMPLEN) {
				int i;
				for (i = 0; i < IMPLEN; i++)
					audioout(0);
				audioout(0);
				gate = 0;
				imp = 0;
				return;
			}
		}
	}

}

void demod(complex float V)
{

	static float fbuf[64];
	static int fidx = 0;
	static int ds = 0;
	static float pPhy=0;

	static float lv=0;

	float l=cabsf(V);
	lv+=l*l;

	if(fmdemod==0) {
		fbuf[fidx] = l;
	} else {
		float phy=cargf(V);
		float dphy=phy-pPhy;

                if (dphy > M_PI) dphy -= 2 * M_PI;
                if (dphy < -M_PI) dphy += 2 * M_PI;
	
		fbuf[fidx] = dphy/(M_PI*5000.0/FSINT);

		pPhy=phy;
		
	}
	fidx = (fidx + 1) % 64;

	/* polyphase filter */
	ds += 8;
	if (ds >= 25) {
		int k, i;
		double M;

		lv=8*lv/ds;
		ds = ds % 25;

		M = 0;
		for (i = fidx, k = ds; k < FILTER_NUM; k += 8, i = (i + 1) % 64)
			M += fbuf[i] * filter[k];

		squelch(M,lv);
		lv=0;
	}
}
