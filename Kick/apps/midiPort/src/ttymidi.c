/*
    This file is part of ttymidi.

    ttymidi is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    ttymidi is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with ttymidi.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <argp.h>
#include <alsa/asoundlib.h>
#include <signal.h>
#include <pthread.h>
// Linux-specific: termios2 sets any baud rate (MIDI's 31250 is not a Bxxx one)
#include <asm/termbits.h>

#define MAX_DEV_STR_LEN              256
#define MAX_MSG_SIZE                1024

// MIDI runs at 31250 baud.
#define MIDI_BAUD_RATE             31250

static volatile sig_atomic_t run = 1;
static int serial = -1;
static int port_out_id;

/* --------------------------------------------------------------------- */
// Program options

static struct argp_option options[] = 
{
	{"serialdevice" , 's', "DEV" , 0, "Serial device to use. Default = /dev/ttyUSB0" },
	{"baudrate"     , 'b', "BAUD", 0, "Serial port baud rate. Default = 31250 (MIDI)" },
	{"verbose"      , 'v', 0     , 0, "For debugging: Produce verbose output" },
	{"printonly"    , 'p', 0     , 0, "Super debugging: Print values read from serial -- and do nothing else" },
	{"quiet"        , 'q', 0     , 0, "Don't produce any output, even when the print command is sent" },
	{"name"		, 'n', "NAME", 0, "Name of the Alsa MIDI client. Default = KICK" },
	{ 0 }
};

typedef struct _arguments
{
	int  silent, verbose, printonly;
	char serialdevice[MAX_DEV_STR_LEN];
	int  baudrate;
	char name[MAX_DEV_STR_LEN];
} arguments_t;

static void exit_cli(int sig)
{
	(void)sig;
	/* Only async-signal-safe work here: the threads see this and stop. */
	run = 0;
}

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	/* Get the input argument from argp_parse, which we
	   know is a pointer to our arguments structure. */
	arguments_t *arguments = state->input;
	char *end;
	long baud;

	switch (key)
	{
		case 'p':
			arguments->printonly = 1;
			break;
		case 'q':
			arguments->silent = 1;
			break;
		case 'v':
			arguments->verbose = 1;
			break;
		case 's':
			if (arg == NULL) break;
			snprintf(arguments->serialdevice, MAX_DEV_STR_LEN, "%s", arg);
			break;
		case 'n':
			if (arg == NULL) break;
			snprintf(arguments->name, MAX_DEV_STR_LEN, "%s", arg);
			break;
		case 'b':
			if (arg == NULL) break;
			errno = 0;
			baud = strtol(arg, &end, 0);
			if (errno != 0 || *end != '\0' || baud <= 0 || baud > 4000000)
				argp_error(state, "Baud rate %s is not supported.", arg);
			arguments->baudrate = (int)baud;
			break;

		case ARGP_KEY_ARG:
		case ARGP_KEY_END:
			break;

		default:
			return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

static void arg_set_defaults(arguments_t *arguments)
{
	arguments->printonly    = 0;
	arguments->silent       = 0;
	arguments->verbose      = 0;
	arguments->baudrate     = MIDI_BAUD_RATE;
	snprintf(arguments->serialdevice, MAX_DEV_STR_LEN, "%s", "/dev/ttyUSB0");
	snprintf(arguments->name, MAX_DEV_STR_LEN, "%s", "KICK");
}

const char *argp_program_version     = "ttymidi 0.60";
const char *argp_program_bug_address = "tvst@hotmail.com";
static char doc[]       = "ttymidi - Connect serial port devices to ALSA MIDI programs!";
static struct argp argp = { options, parse_opt, 0, doc };
arguments_t arguments;



/* --------------------------------------------------------------------- */
// MIDI stuff

static int open_seq(snd_seq_t** seq) 
{
	int out_id, in_id;

	if (snd_seq_open(seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) 
	{
		fprintf(stderr, "Error opening ALSA sequencer.\n");
		exit(1);
	}

	snd_seq_set_client_name(*seq, arguments.name);

	/* What arrives on the serial port is read from this port... */
	if ((out_id = snd_seq_create_simple_port(*seq, "KICK",
					SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ,
					SND_SEQ_PORT_TYPE_MIDI_GENERIC|SND_SEQ_PORT_TYPE_APPLICATION)) < 0) 
	{
		fprintf(stderr, "Error creating sequencer port.\n");
		exit(1);
	}

	/* ...and what is written to this one goes out of it. */
	if ((in_id = snd_seq_create_simple_port(*seq, "KICK",
					SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE,
					SND_SEQ_PORT_TYPE_MIDI_GENERIC|SND_SEQ_PORT_TYPE_APPLICATION)) < 0) 
	{
		fprintf(stderr, "Error creating sequencer port.\n");
		exit(1);
	}

	return out_id;
}

static void print_bytes(const char *direction, const unsigned char *bytes, long count)
{
	long i;

	if (arguments.silent || !arguments.verbose)
		return;

	printf("%s", direction);
	for (i = 0; i < count && i < 16; ++i)
		printf(" %02x", bytes[i]);
	if (count > 16)
		printf(" ... (%ld bytes)", count);
	printf("\n");
}

static int write_all(int fd, const unsigned char *data, size_t length)
{
	while (length > 0)
	{
		ssize_t written = write(fd, data, length);

		if (written < 0)
		{
			if (errno == EINTR)
				continue;
			return -1;
		}

		data   += written;
		length -= (size_t)written;
	}

	return 0;
}

/*
 * ALSA -> serial. The decoder turns any sequencer event that has a MIDI
 * equivalent into its bytes: channel messages (poly aftertouch is 0xA0),
 * system common and realtime messages and SysEx. Every message is sent
 * with its status byte.
 */
static void write_midi_action_to_serial_port(snd_seq_t* seq_handle, snd_midi_event_t* decoder)
{
	snd_seq_event_t* ev;
	unsigned char bytes[16];
	long count;
	int result;

	do 
	{
		result = snd_seq_event_input(seq_handle, &ev);
		if (result == -ENOSPC)
		{
			if (!arguments.silent)
				fprintf(stderr, "ALSA input overrun: events were lost\n");
			continue;
		}
		if (result < 0 || ev == NULL)
			break;

		if (ev->type == SND_SEQ_EVENT_SYSEX)
		{
			/* As it is, and in one piece however long it is. */
			print_bytes("Alsa   ", ev->data.ext.ptr, (long)ev->data.ext.len);
			if (write_all(serial, ev->data.ext.ptr, ev->data.ext.len) < 0)
				perror("write");
			continue;
		}

		count = snd_midi_event_decode(decoder, bytes, sizeof(bytes), ev);
		snd_midi_event_reset_decode(decoder);

		/* Events without a MIDI equivalent (port subscriptions...) */
		if (count <= 0)
			continue;

		print_bytes("Alsa   ", bytes, count);
		if (write_all(serial, bytes, (size_t)count) < 0)
			perror("write");

	} while (snd_seq_event_input_pending(seq_handle, 0) > 0);
}


static void* read_midi_from_alsa(void* seq) 
{
	int npfd;
	struct pollfd* pfd;
	snd_seq_t* seq_handle;
	snd_midi_event_t* decoder;

	seq_handle = seq;

	if (snd_midi_event_new(MAX_MSG_SIZE, &decoder) < 0)
	{
		fprintf(stderr, "Error creating the MIDI decoder.\n");
		run = 0;
		return NULL;
	}
	snd_midi_event_no_status(decoder, 1);

	npfd = snd_seq_poll_descriptors_count(seq_handle, POLLIN);
	pfd = (struct pollfd*) alloca(npfd * sizeof(struct pollfd));
	snd_seq_poll_descriptors(seq_handle, pfd, npfd, POLLIN);	

	while (run) 
	{
		if (poll(pfd, npfd, 100) > 0) 
		{
			write_midi_action_to_serial_port(seq_handle, decoder);
		}
	}	

	snd_midi_event_free(decoder);

	if (!arguments.silent)
		printf("\nStopping [PC]->[Hardware] communication...");
	return NULL;
}

/*
 * Serial -> ALSA. The encoder takes the bytes as they come: it follows
 * running status, passes realtime messages that arrive in the middle of
 * another message, and collects SysEx (in pieces of MAX_MSG_SIZE bytes).
 */
static void* read_midi_from_serial_port(void* seq) 
{
	snd_seq_t* seq_handle = seq;
	snd_midi_event_t* encoder;
	snd_seq_event_t ev;
	struct pollfd pfd;
	unsigned char buf[256];
	ssize_t length, i;
	int result;

	if (snd_midi_event_new(MAX_MSG_SIZE, &encoder) < 0)
	{
		fprintf(stderr, "Error creating the MIDI encoder.\n");
		run = 0;
		return NULL;
	}

	pfd.fd     = serial;
	pfd.events = POLLIN;

	while (run) 
	{
		result = poll(&pfd, 1, 100);
		if (result < 0)
		{
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (result == 0)
			continue;

		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
		{
			fprintf(stderr, "%s: the serial port went away\n", arguments.serialdevice);
			break;
		}

		length = read(serial, buf, sizeof(buf));
		if (length < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			perror(arguments.serialdevice);
			break;
		}
		if (length == 0)
			continue;

		/* 
		 * super-debug mode: only print to screen whatever
		 * comes through the serial port.
		 */
		if (arguments.printonly) 
		{
			for (i = 0; i < length; ++i)
				printf("%x\t", buf[i]);
			fflush(stdout);
			continue;
		}

		for (i = 0; i < length; ++i)
		{
			snd_seq_ev_clear(&ev);
			if (snd_midi_event_encode_byte(encoder, buf[i], &ev) != 1)
				continue;

			snd_seq_ev_set_direct(&ev);
			snd_seq_ev_set_source(&ev, port_out_id);
			snd_seq_ev_set_subs(&ev);

			if (arguments.verbose && !arguments.silent)
				printf("Serial  event type %d\n", ev.type);

			snd_seq_event_output_direct(seq_handle, &ev);
		}
	}

	snd_midi_event_free(encoder);
	run = 0;
	return NULL;
}

/* --------------------------------------------------------------------- */
// Serial port

static int setup_serial_port(int fd, int baudrate, struct termios2 *saved)
{
	struct termios2 tio;

	/* save current serial port settings, to restore them on exit */
	if (ioctl(fd, TCGETS2, saved) < 0)
	{
		perror("TCGETS2");
		return -1;
	}

	tio = *saved;

	/* Raw 8N1 without flow control, at any rate (BOTHER). */
	tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY | IMAXBEL);
	tio.c_oflag &= ~OPOST;
	tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS | CBAUD | (CBAUD << IBSHIFT));
	tio.c_cflag |= CS8 | CREAD | CLOCAL | BOTHER | (BOTHER << IBSHIFT);
	tio.c_ispeed = (speed_t)baudrate;
	tio.c_ospeed = (speed_t)baudrate;
	tio.c_cc[VMIN]  = 1;
	tio.c_cc[VTIME] = 0;

	if (ioctl(fd, TCSETS2, &tio) < 0)
	{
		perror("TCSETS2");
		return -1;
	}

	ioctl(fd, TCFLSH, TCIFLUSH);
	return 0;
}

/* --------------------------------------------------------------------- */
// Main program

int main(int argc, char** argv)
{
	struct termios2 oldtio;
	struct sigaction action;
	pthread_t midi_out_thread, midi_in_thread;
	snd_seq_t *seq;

	arg_set_defaults(&arguments);
	argp_parse(&argp, argc, argv, 0, 0, &arguments);

	/*
	 * Open MIDI output port
	 */

	port_out_id = open_seq(&seq);

	/* 
	 *  Open modem device for reading and not as controlling tty because we don't
	 *  want to get killed if linenoise sends CTRL-C.
	 */
	
	serial = open(arguments.serialdevice, O_RDWR | O_NOCTTY ); 

	if (serial < 0) 
	{
		perror(arguments.serialdevice); 
		snd_seq_close(seq);
		return 1;
	}

	if (setup_serial_port(serial, arguments.baudrate, &oldtio) < 0)
	{
		close(serial);
		snd_seq_close(seq);
		return 1;
	}

	if (arguments.printonly) 
	{
		printf("Super debug mode: Only printing the signal to screen. Nothing else.\n");
	}

	memset(&action, 0, sizeof(action));
	action.sa_handler = exit_cli;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	/* 
	 * read commands: one thread polls the ALSA port, the other the serial
	 * port; both look at run every 100 ms.
	 */
	if (pthread_create(&midi_out_thread, NULL, read_midi_from_alsa, (void*) seq) != 0
	 || pthread_create(&midi_in_thread, NULL, read_midi_from_serial_port, (void*) seq) != 0)
	{
		fprintf(stderr, "Error starting the threads.\n");
		run = 0;
		ioctl(serial, TCSETS2, &oldtio);
		close(serial);
		snd_seq_close(seq);
		return 1;
	}

	pthread_join(midi_in_thread, NULL);
	run = 0;
	pthread_join(midi_out_thread, NULL);

	/* restore the old port settings */
	ioctl(serial, TCSETS2, &oldtio);
	close(serial);
	snd_seq_close(seq);

	if (!arguments.silent)
		printf("\ndone!\n");
	return 0;
}
