#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>

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

#define DEFAULT_CAN_TX_BUFFER_SIZE 1024*1024
#define DEFAULT_UDP_TX_BUFFER_SIZE 1024*1024

int can_sock;
int udp_sock;

typedef struct tx_can_frame_struct {
	struct can_frame frame;
	bool sent;
	pthread_mutex_t mutex;
} tx_can_frame_t;

typedef struct tx_buffer_struct {
	int buffer_size;
	int next_frame;
	tx_can_frame_t * frames;
} tx_buffer_t;

tx_buffer_t tx_can_buffer;
tx_buffer_t tx_udp_buffer;

pthread_t rx_can_thread_id = 0;
pthread_t tx_udp_thread_id = 0;
pthread_t rx_udp_thread_id = 0;
pthread_t tx_can_thread_id = 0;

void * tx_udp_thread(void *p) {
	uint64_t cnt = 0;
	int current_frame = (int)p;
	tx_can_frame_t *frame;

//	for ( ; cnt == 0 || current_frame != tx_can_buffer.next_frame ; current_frame++)
	for ( ; ; current_frame++)
	{
		if (current_frame == tx_can_buffer.buffer_size) {
			current_frame = 0;
		}
		printf("tx: frame %d\n", current_frame);
		
		frame = &tx_can_buffer.frames[current_frame];

		pthread_mutex_lock(&frame->mutex);
		
		if (frame->sent == true) {
			fprintf(stderr, "trying to send already sent frame %d\n", current_frame);
			pthread_mutex_unlock(&frame->mutex);
	//		return NULL;
			continue;
		}

		printf("CAN_ID = 0x%x\n", frame->frame.can_id);
		printf("DATA = ");
		for (int i = 0; i < frame->frame.can_dlc; ++i) {
			printf("0x%02x ", frame->frame.data[i]);
		}
		printf("\n");

		frame->frame.can_id = htonl(frame->frame.can_id);

		/* send here */

		frame->sent = true;

		pthread_mutex_unlock(&frame->mutex);

		cnt++;
		printf("tid = %ld CNT = %lld\n", pthread_self(), cnt);
	}

	return NULL;	
}

void * rx_can_thread(void *p) {
	uint64_t cnt = 0;
	int nbytes;
	tx_can_frame_t *frame;

	for ( ; ; tx_can_buffer.next_frame++) {
		if (tx_can_buffer.next_frame == tx_can_buffer.buffer_size) {
			tx_can_buffer.next_frame = 0;
		}
		printf("rx: frame %d\n", tx_can_buffer.next_frame);

		frame = &tx_can_buffer.frames[tx_can_buffer.next_frame];

		pthread_mutex_lock(&frame->mutex);

		if (!frame->sent) {
			fprintf(stderr, "CAN rx buffer overflow\n");
			exit(EXIT_FAILURE);
		}

		frame->sent = false;

		nbytes = read(can_sock, &frame->frame, sizeof(struct can_frame));

		if (nbytes < 0) {
			perror("can socket read");
			exit(EXIT_FAILURE);
		}

		if (nbytes < sizeof(struct can_frame)) {
			fprintf(stderr, "read: incomplete CAN frame\n");
			exit(EXIT_FAILURE);
		}

		if (tx_udp_thread_id == 0 || !pthread_tryjoin_np(tx_udp_thread_id, NULL)) {
			pthread_create(&tx_udp_thread_id, NULL, &tx_udp_thread, (void *)tx_can_buffer.next_frame);
		}

		cnt++;

		printf("RCVD = %lld\n", cnt);

		pthread_mutex_unlock(&frame->mutex);
	}

	return NULL;
}

int main(int argc, char *argv[]) {
	struct sockaddr_can addr;
	struct ifreq ifr;

	tx_can_buffer.frames = calloc(DEFAULT_CAN_TX_BUFFER_SIZE, sizeof(tx_can_frame_t));
	if (tx_can_buffer.frames == NULL) {
		perror("calloc");
		return EXIT_FAILURE;
	}
	tx_can_buffer.next_frame = 0;
	tx_can_buffer.buffer_size = DEFAULT_CAN_TX_BUFFER_SIZE;

	for (int i = 0; i < DEFAULT_CAN_TX_BUFFER_SIZE; ++i) {
		tx_can_buffer.frames[i].sent = true;
		pthread_mutex_init(&tx_can_buffer.frames[i].mutex, NULL);
	}

	can_sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);

	if (can_sock < 0) {
		perror("can socket create");
		return EXIT_FAILURE;
	}

	strcpy(ifr.ifr_name, "can0" );
	if (ioctl(can_sock, SIOCGIFINDEX, &ifr)) {
		perror("can socket ioctl interface set");
		return EXIT_FAILURE;
	}

	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	if (bind(can_sock, (struct sockaddr *)&addr, sizeof(addr))) {
		perror("can socket bind");
		return EXIT_FAILURE;
	}

	if (pthread_create(&rx_can_thread_id, NULL, &rx_can_thread, NULL)) {
		perror("pthread_create");
		return EXIT_FAILURE;
	}

	pthread_join(rx_can_thread_id, NULL);

	free(tx_can_buffer.frames);

	return EXIT_SUCCESS;
}
