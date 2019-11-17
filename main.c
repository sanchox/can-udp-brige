#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <argp.h>

#include <netdb.h>
#include <ifaddrs.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <linux/net.h>
#include <linux/if_link.h>
#include <linux/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>

const char *argp_program_version =
		"can2udp 0.1";
const char *argp_program_bug_address =
		"<alexandergusarov@gmail.com>";

/* Program documentation. */
static char doc[] =
		"CAN<->UDP transport";

/* The options we understand. */
static struct argp_option options[] = {
		{ "can-interface", 'i', "INTERFACE", 0, "CAN interface name", 0 },
		{ "remote-ip-addr", 'a', "R_ADDRESS", 0, "Remote server IP address", 0 },
		{ "remote-udp-port", 'p', "R_PORT", 0, "Remote server UDP port", 0 },
		{ "local-udp-port", 'P', "PORT", 0,	"Local UDP port", 0 },
		{ "buffer-size", 'b', "BUF_SIZE", 0, "Buffer size (optional)", 0 },
		{ 0 }
};

/* Program configuration */
struct arguments {
	char *can_interface_name;
	bool udp_tx_ip_set;
	struct in_addr udp_tx_ip;
	uint16_t udp_tx_port;
	uint16_t udp_rx_port;
	int buffer_size;
};

#define DEFAULT_BUFFER_SIZE (256 * 1024)

typedef struct frame_struct {
	struct can_frame frame;
	bool sent;
	pthread_mutex_t mutex;
} frame_t;

typedef struct buffer_struct {
	int buffer_size;
	int next_frame;
	frame_t *frames;
} buffer_t;

typedef struct ctx_struct {
	buffer_t buffer;
	int tx_socket;
	int rx_socket;
	uint64_t rx_cnt;
	uint64_t tx_cnt;
	pthread_t rx_thread_id;
	pthread_t tx_thread_id;
	bool udp_server;
} ctx_t;

ctx_t can_rx_udp_tx;
ctx_t udp_rx_can_tx;

int check_interface_name(const char *interface_name);
void* rx_thread(void *p);
void* tx_thread(void *p);

/* Check input string for valid network interface name */
int check_interface_name(const char *interface_name) {
	if (interface_name == NULL) {
		printf("Interface name is NULL\n");
		return (0);
	}

	if (strlen(interface_name) > IFNAMSIZ) {
		printf("Interface name is too long\n");
		return (0);
	}

	/* Get network interfaces list */
	struct ifaddrs *ifaddr;
	if (getifaddrs(&ifaddr) == -1)
	{
		perror("getifaddrs()");
		exit(EXIT_FAILURE);
	}

	/* Cycle through to find name match*/
	for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_name != NULL
				&& strcmp(interface_name, ifa->ifa_name) == 0) {
			/* matched */
			freeifaddrs(ifaddr);
			return (1);
		}
	}
	return (0);
}

/* Parse a single option. */
static error_t parse_opt(int key, char *arg, struct argp_state *state) {
	/*
	 * Get the input argument from argp_parse, which we
	 * know is a pointer to our arguments structure.
	 */
	struct arguments *arguments = state->input;

	switch (key) {
	case 'I': {
		if (!check_interface_name(arg))
			argp_error(state, "No such interface: %s", arg);
		arguments->can_interface_name = arg;
	}
		break;
	case 'i': {
		if (!inet_aton(arg, &arguments->udp_tx_ip))
			argp_error(state, "Invalid IP address: %s", arg);
		arguments->udp_tx_ip_set = true;
	}
		break;
	case 'p': {
		int port = atoi(arg);
		if (port < 1 || port > 65535)
			argp_error(state, "Invalid UDP port: %d", port);
		arguments->udp_tx_port = port;
	}
		break;
	case 'P': {
		int port = atoi(arg);
		if (port < 1 || port > 65535)
			argp_error(state, "Invalid UDP port: %d", port);
		arguments->udp_rx_port = port;
	}
		break;
	case 'b': {
		int buf_size = atoi(arg);
		if (buf_size < DEFAULT_BUFFER_SIZE)
			argp_error(state, "Invalid buffer size: %d", buf_size);
		arguments->buffer_size = buf_size;
	}
		break;
	case ARGP_KEY_NO_ARGS:
		argp_usage(state);
		break;
	default:
		return (ARGP_ERR_UNKNOWN);
	}
	return (0);
}

void* tx_thread(void *p) {
	ctx_t *ctx = (ctx_t*) p;
	for (int current_frame = 0;; current_frame++) {
		int nbytes;
		frame_t *frame;

		if (current_frame == ctx->buffer.buffer_size) {
			current_frame = 0;
		}
		frame = &ctx->buffer.frames[current_frame];

		pthread_mutex_lock(&frame->mutex);

		if (frame->sent == true) {
			pthread_mutex_unlock(&frame->mutex);
			continue;
		}

		if (!ctx->udp_server)
			frame->frame.can_id = htonl(frame->frame.can_id);

		nbytes = write(ctx->tx_socket, &frame->frame, sizeof(struct can_frame));

		if (nbytes < 0) {
			perror("tx socket write");
			exit(EXIT_FAILURE);
		}

		if (nbytes < sizeof(struct can_frame)) {
			fprintf(stderr, "write: incomplete write can frame\n");
			exit(EXIT_FAILURE);
		}

		frame->sent = true;

		pthread_mutex_unlock(&frame->mutex);

		ctx->tx_cnt++;

		printf("%s_tx: current_frame = %d frames_transmitted = %lu\n",
				ctx->udp_server ? "can" : "udp", current_frame, ctx->tx_cnt);
	}

	return NULL;
}

void* rx_thread(void *p) {
	ctx_t *ctx = (ctx_t*) p;
	for (;; ctx->buffer.next_frame++) {
		int nbytes;
		frame_t *frame;

		if (ctx->buffer.next_frame == ctx->buffer.buffer_size) {
			ctx->buffer.next_frame = 0;
		}

		frame = &ctx->buffer.frames[ctx->buffer.next_frame];

		pthread_mutex_lock(&frame->mutex);

		if (!frame->sent) {
			fprintf(stderr, "buffer overflow\n");
			exit(EXIT_FAILURE);
		}

		frame->sent = false;

		nbytes = read(ctx->rx_socket, &frame->frame, sizeof(struct can_frame));

		if (nbytes < 0) {
			perror("rx socket read");
			exit(EXIT_FAILURE);
		}

		if (nbytes < sizeof(struct can_frame)) {
			fprintf(stderr, "read: incomplete CAN frame\n");
			exit(EXIT_FAILURE);
		}

		if (ctx->udp_server)
			frame->frame.can_id = ntohl(frame->frame.can_id);

		pthread_mutex_unlock(&frame->mutex);

		ctx->rx_cnt++;

		printf("%s_rx: current_frame = %d frames_received = %lu\n",
				ctx->udp_server ? "udp" : "can", ctx->buffer.next_frame,
				ctx->rx_cnt);
	}

	return NULL;
}

int main(int argc, char *argv[]) {
	/* Initializing arguments structure */
	struct arguments arguments =
	{
		.can_interface_name = NULL,
		.udp_tx_ip_set = false,
		.udp_tx_port = 0,
		.udp_rx_port = 0,
		.buffer_size = DEFAULT_BUFFER_SIZE
	};

	/* Parsing command line arguments */
	struct argp argp = {options, parse_opt, NULL, doc, 0, 0, 0};
	if (argp_parse(&argp, argc, argv, 0, 0, &arguments))
	{
		perror("argp_parse()");
		return EXIT_FAILURE;
	}

	/* Initializing can_rx_udp_tx buffer */
	can_rx_udp_tx.buffer.buffer_size = arguments.buffer_size;
	can_rx_udp_tx.buffer.frames = calloc(can_rx_udp_tx.buffer.buffer_size,
			sizeof(frame_t));
	if (can_rx_udp_tx.buffer.frames == NULL) {
		perror("calloc");
		return EXIT_FAILURE;
	}
	can_rx_udp_tx.buffer.next_frame = 0;
	for (int i = 0; i < can_rx_udp_tx.buffer.buffer_size; ++i) {
		can_rx_udp_tx.buffer.frames[i].sent = true;
		pthread_mutex_init(&can_rx_udp_tx.buffer.frames[i].mutex, NULL);
	}

	/* Initializing udp_rx_can_tx buffer */
	udp_rx_can_tx.buffer.buffer_size = arguments.buffer_size;
	udp_rx_can_tx.buffer.frames = calloc(udp_rx_can_tx.buffer.buffer_size,
			sizeof(frame_t));
	if (udp_rx_can_tx.buffer.frames == NULL) {
		perror("calloc");
		return EXIT_FAILURE;
	}
	udp_rx_can_tx.buffer.next_frame = 0;
	for (int i = 0; i < udp_rx_can_tx.buffer.buffer_size; ++i) {
		udp_rx_can_tx.buffer.frames[i].sent = true;
		pthread_mutex_init(&udp_rx_can_tx.buffer.frames[i].mutex, NULL);
	}

	// FIXME: Variables for init can_sock
	struct ifreq can_interface;
	struct sockaddr_can can_addr;

	/* Initializing can_sock */
	int can_socket = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (can_socket < 0) {
		perror("can socket create");
		return EXIT_FAILURE;
	}
	strcpy(can_interface.ifr_name, arguments.can_interface_name);
	if (ioctl(can_socket, SIOCGIFINDEX, &can_interface)) {
		perror("can socket ioctl interface set");
		return EXIT_FAILURE;
	}
	can_addr.can_family = AF_CAN;
	can_addr.can_ifindex = can_interface.ifr_ifindex;
	if (bind(can_socket, (struct sockaddr*) &can_addr, sizeof(can_addr))) {
		perror("can socket bind");
		return EXIT_FAILURE;
	}
	can_rx_udp_tx.rx_socket = can_socket;
	udp_rx_can_tx.tx_socket = can_socket;

	// FIXME: Variables for init rx_udp_sock
	struct sockaddr_in rx_udp_addr;

	/* Initializing rx_udp_sock */
	int rx_udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (rx_udp_socket < 0) {
		perror("udp socket create");
		return EXIT_FAILURE;
	}
	rx_udp_addr.sin_family = AF_INET;
	rx_udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	rx_udp_addr.sin_port = htons(arguments.udp_rx_port);

	if (bind(rx_udp_socket, (struct sockaddr*) &rx_udp_addr,
			sizeof(rx_udp_addr))) {
		perror("udp socket bind");
		return EXIT_FAILURE;
	}
	udp_rx_can_tx.rx_socket = rx_udp_socket;
	udp_rx_can_tx.udp_server = true;

	// FIXME: Variables for init tx_udp_sock
	struct sockaddr_in tx_udp_addr;

	/* Initializing tx_udp_sock */
	int tx_udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (tx_udp_socket < 0) {
		perror("udp socket create");
		return EXIT_FAILURE;
	}
	tx_udp_addr.sin_family = AF_INET;
	tx_udp_addr.sin_addr = arguments.udp_tx_ip;
	tx_udp_addr.sin_port = htons(arguments.udp_tx_port);

	if (connect(tx_udp_socket, (struct sockaddr*) &tx_udp_addr,
			sizeof(tx_udp_addr)) < 0) {
		perror("udp socket connect");
		return EXIT_FAILURE;
	}
	can_rx_udp_tx.tx_socket = tx_udp_socket;
	can_rx_udp_tx.udp_server = false;

	/* Threads start */
	if (pthread_create(&can_rx_udp_tx.tx_thread_id, NULL, &tx_thread,
			&can_rx_udp_tx)) {
		perror("udp_tx_pthread_create");
		return EXIT_FAILURE;
	}
	if (pthread_create(&can_rx_udp_tx.rx_thread_id, NULL, &rx_thread,
			&can_rx_udp_tx)) {
		perror("can_rx_pthread_create");
		return EXIT_FAILURE;
	}
	if (pthread_create(&udp_rx_can_tx.tx_thread_id, NULL, &tx_thread,
			&udp_rx_can_tx)) {
		perror("can_tx_pthread_create");
		return EXIT_FAILURE;
	}
	if (pthread_create(&udp_rx_can_tx.rx_thread_id, NULL, &rx_thread,
			&udp_rx_can_tx)) {
		perror("udp_rx_pthread_create");
		return EXIT_FAILURE;
	}

	/* Threads stop */
	pthread_join(can_rx_udp_tx.rx_thread_id, NULL);
	pthread_join(can_rx_udp_tx.tx_thread_id, NULL);
	pthread_join(udp_rx_can_tx.rx_thread_id, NULL);
	pthread_join(udp_rx_can_tx.tx_thread_id, NULL);

	/* Cleanup */
	free(can_rx_udp_tx.buffer.frames);
	free(udp_rx_can_tx.buffer.frames);

	return EXIT_SUCCESS;
}
