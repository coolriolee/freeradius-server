/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file protocols/radius/packet.c
 * @brief Functions to deal with fr_radius_packet_t data structures.
 *
 * @copyright 2000-2017 The FreeRADIUS server project
 */
RCSID("$Id$")

#include "attrs.h"

#include <freeradius-devel/util/udp.h>
#include <freeradius-devel/util/syserror.h>

#include <fcntl.h>

typedef struct {
	uint8_t	code;
	uint8_t	id;
	uint8_t	length[2];
	uint8_t	vector[RADIUS_AUTH_VECTOR_LENGTH];
	uint8_t	data[1];
} radius_packet_t;


/*
 *	Some messages get printed out only in debugging mode.
 */
#define FR_DEBUG_STRERROR_PRINTF if (fr_debug_lvl) fr_strerror_printf_push


/** Encode a packet
 *
 */
ssize_t fr_radius_packet_encode(fr_radius_packet_t *packet, fr_pair_list_t *list,
				fr_radius_packet_t const *original, char const *secret)
{
	uint8_t const *original_data;
	ssize_t slen;

	/*
	 *	A 4K packet, aligned on 64-bits.
	 */
	uint8_t	data[MAX_PACKET_LEN];

#ifndef NDEBUG
	if (fr_debug_lvl >= L_DBG_LVL_4) fr_radius_packet_log_hex(&default_log, packet);
#endif

	if (original) {
		original_data = original->data;
	} else {
		original_data = NULL;
	}

	/*
	 *	This has to be initialized for Access-Request packets
	 */
	memcpy(data + 4, packet->vector, sizeof(packet->vector));

	slen = fr_radius_encode(data, sizeof(data), original_data, secret, talloc_array_length(secret) - 1,
				packet->code, packet->id, list);
	if (slen < 0) return slen;

	/*
	 *	Fill in the rest of the fields, and copy the data over
	 *	from the local stack to the newly allocated memory.
	 *
	 *	Yes, all this 'memcpy' is slow, but it means
	 *	that we only allocate the minimum amount of
	 *	memory for a request.
	 */
	packet->data_len = (size_t) slen;
	packet->data = talloc_array(packet, uint8_t, packet->data_len);
	if (!packet->data) {
		fr_strerror_const("Out of memory");
		return -1;
	}

	memcpy(packet->data, data, packet->data_len);

	return 0;
}

/** See if the data pointed to by PTR is a valid RADIUS packet.
 *
 * Packet is not 'const * const' because we may update data_len, if there's more data
 * in the UDP packet than in the RADIUS packet.
 *
 * @param[in] packet		to check.
 * @param[in] max_attributes	to decode.
 * @param[in] require_ma	to require Message-Authenticator.
 * @param[out] reason		if not NULL, will have the failure reason written to where it points.
 * @return
 *	- True on success.
 *	- False on failure.
 */
bool fr_radius_packet_ok(fr_radius_packet_t *packet, uint32_t max_attributes, bool require_ma, decode_fail_t *reason)
{
	char host_ipaddr[INET6_ADDRSTRLEN];

	if (!fr_radius_ok(packet->data, &packet->data_len, max_attributes, require_ma, reason)) {
		FR_DEBUG_STRERROR_PRINTF("Bad packet received from host %s",
					 inet_ntop(packet->socket.inet.src_ipaddr.af, &packet->socket.inet.src_ipaddr.addr,
						   host_ipaddr, sizeof(host_ipaddr)));
		return false;
	}

	/*
	 *	Fill RADIUS header fields
	 */
	packet->code = packet->data[0];
	packet->id = packet->data[1];
	memcpy(packet->vector, packet->data + 4, sizeof(packet->vector));
	return true;
}


/** Verify the Request/Response Authenticator (and Message-Authenticator if present) of a packet
 *
 */
int fr_radius_packet_verify(fr_radius_packet_t *packet, fr_radius_packet_t *original, char const *secret)
{	
	char		buffer[INET6_ADDRSTRLEN];

	if (!packet->data) return -1;

	if (fr_radius_verify(packet->data, original ? original->data + 4 : NULL,
			     (uint8_t const *) secret, talloc_array_length(secret) - 1, false) < 0) {
		fr_strerror_printf_push("Received invalid packet from %s",
					inet_ntop(packet->socket.inet.src_ipaddr.af, &packet->socket.inet.src_ipaddr.addr,
						  buffer, sizeof(buffer)));
		return -1;
	}

	return 0;
}


/** Sign a previously encoded packet
 *
 */
int fr_radius_packet_sign(fr_radius_packet_t *packet, fr_radius_packet_t const *original,
			  char const *secret)
{
	int ret;

	/*
	 *	Copy the random vector to the packet.  Other packet
	 *	codes have the Request Authenticator be the packet
	 *	signature.
	 */
	if ((packet->code == FR_RADIUS_CODE_ACCESS_REQUEST) ||
	    (packet->code == FR_RADIUS_CODE_STATUS_SERVER)) {
		memcpy(packet->data + 4, packet->vector, sizeof(packet->vector));
	}

	ret = fr_radius_sign(packet->data, original ? original->data + 4 : NULL,
			       (uint8_t const *) secret, talloc_array_length(secret) - 1);
	if (ret < 0) return ret;

	memcpy(packet->vector, packet->data + 4, RADIUS_AUTH_VECTOR_LENGTH);
	return 0;
}


/** Wrapper for recvfrom, which handles recvfromto, IPv6, and all possible combinations
 *
 */
static ssize_t rad_recvfrom(int sockfd, fr_radius_packet_t *packet, int flags)
{
	ssize_t			data_len;

	data_len = fr_radius_recv_header(sockfd, &packet->socket.inet.src_ipaddr, &packet->socket.inet.src_port, &packet->code);
	if (data_len < 0) {
		if ((errno == EAGAIN) || (errno == EINTR)) return 0;
		return -1;
	}

	if (data_len == 0) return -1; /* invalid packet */

	packet->data = talloc_array(packet, uint8_t, data_len);
	if (!packet->data) return -1;

	packet->data_len = data_len;

	return udp_recv(sockfd, flags, &packet->socket, packet->data, packet->data_len, &packet->timestamp);
}


/** Receive UDP client requests, and fill in the basics of a fr_radius_packet_t structure
 *
 */
fr_radius_packet_t *fr_radius_packet_recv(TALLOC_CTX *ctx, int fd, int flags, uint32_t max_attributes, bool require_ma)
{
	ssize_t			data_len;
	fr_radius_packet_t	*packet;

	/*
	 *	Allocate the new request data structure
	 */
	packet = fr_radius_packet_alloc(ctx, false);
	if (!packet) {
		fr_strerror_const("out of memory");
		return NULL;
	}

	data_len = rad_recvfrom(fd, packet, flags);
	if (data_len < 0) {
		FR_DEBUG_STRERROR_PRINTF("Error receiving packet: %s", fr_syserror(errno));
		fr_radius_packet_free(&packet);
		return NULL;
	}

#ifdef WITH_VERIFY_PTR
	/*
	 *	Double-check that the fields we want are filled in.
	 */
	if ((packet->socket.inet.src_ipaddr.af == AF_UNSPEC) ||
	    (packet->socket.inet.src_port == 0) ||
	    (packet->socket.inet.dst_ipaddr.af == AF_UNSPEC) ||
	    (packet->socket.inet.dst_port == 0)) {
		FR_DEBUG_STRERROR_PRINTF("Error receiving packet: %s", fr_syserror(errno));
		fr_radius_packet_free(&packet);
		return NULL;
	}
#endif

	packet->data_len = data_len; /* unsigned vs signed */

	/*
	 *	If the packet is too big, then rad_recvfrom did NOT
	 *	allocate memory.  Instead, it just discarded the
	 *	packet.
	 */
	if (packet->data_len > MAX_PACKET_LEN) {
		FR_DEBUG_STRERROR_PRINTF("Discarding packet: Larger than RFC limitation of 4096 bytes");
		fr_radius_packet_free(&packet);
		return NULL;
	}

	/*
	 *	Read no data.  Continue.
	 *	This check is AFTER the MAX_PACKET_LEN check above, because
	 *	if the packet is larger than MAX_PACKET_LEN, we also have
	 *	packet->data == NULL
	 */
	if ((packet->data_len == 0) || !packet->data) {
		FR_DEBUG_STRERROR_PRINTF("Empty packet: Socket is not ready");
		fr_radius_packet_free(&packet);
		return NULL;
	}

	/*
	 *	See if it's a well-formed RADIUS packet.
	 */
	if (!fr_radius_packet_ok(packet, max_attributes, require_ma, NULL)) {
		fr_radius_packet_free(&packet);
		return NULL;
	}

	/*
	 *	Remember which socket we read the packet from.
	 */
	packet->socket.fd = fd;

	/*
	 *	FIXME: Do even more filtering by only permitting
	 *	certain IP's.  The problem is that we don't know
	 *	how to do this properly for all possible clients...
	 */

	return packet;
}

/** Reply to the request
 *
 * Also attach reply attribute value pairs and any user message provided.
 */
int fr_radius_packet_send(fr_radius_packet_t *packet, fr_pair_list_t *list,
			  fr_radius_packet_t const *original, char const *secret)
{
	/*
	 *	Maybe it's a fake packet.  Don't send it.
	 */
	if (packet->socket.fd < 0) {
		return 0;
	}

	/*
	 *  First time through, allocate room for the packet
	 */
	if (!packet->data) {
		/*
		 *	Encode the packet.
		 */
		if (fr_radius_packet_encode(packet, list, original, secret) < 0) {
			return -1;
		}

		/*
		 *	Re-sign it, including updating the
		 *	Message-Authenticator.
		 */
		if (fr_radius_packet_sign(packet, original, secret) < 0) {
			return -1;
		}

		/*
		 *	If packet->data points to data, then we print out
		 *	the VP list again only for debugging.
		 */
	}

	/*
	 *	If the socket is TCP, call write().  Calling sendto()
	 *	is allowed on some platforms, but it's not nice.
	 */
	if (packet->socket.type == SOCK_STREAM) {
		ssize_t ret;

		ret = write(packet->socket.fd, packet->data, packet->data_len);
		if (ret >= 0) return ret;

		fr_strerror_printf("sendto failed: %s", fr_syserror(errno));
		return -1;
	}

	/*
	 *	And send it on it's way.
	 *
	 *	No need to call fr_socket_addr_swap as apparently
	 *	the address is already inverted.
	 */
	return udp_send(&packet->socket, 0, packet->data, packet->data_len);
}

void _fr_radius_packet_log_hex(fr_log_t const *log, fr_radius_packet_t const *packet, char const *file, int line)
{
	uint8_t const *attr, *end;
	char buffer[1024];

	if (!packet->data) return;

	fr_log(log, L_DBG, file, line, "  Socket   : %d", packet->socket.fd);
	fr_log(log, L_DBG, file, line, "  Proto    : %d", (packet->socket.type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP);

	if ((packet->socket.inet.src_ipaddr.af == AF_INET) || (packet->socket.inet.src_ipaddr.af == AF_INET6)) {
		fr_log(log, L_DBG, file, line, "  Src IP   : %pV", fr_box_ipaddr(packet->socket.inet.src_ipaddr));
		fr_log(log, L_DBG, file, line, "  Src Port : %u", packet->socket.inet.src_port);
		fr_log(log, L_DBG, file, line, "  Dst IP   : %pV", fr_box_ipaddr(packet->socket.inet.dst_ipaddr));
		fr_log(log, L_DBG, file, line, "  Dst Port : %u", packet->socket.inet.dst_port);
	}

       if ((packet->data[0] > 0) && (packet->data[0] < FR_RADIUS_CODE_MAX)) {
               fr_log(log, L_DBG, file, line, "  Code     : %s", fr_radius_packet_names[packet->data[0]]);
       } else {
               fr_log(log, L_DBG, file, line, "  Code     : %u", packet->data[0]);
       }

       fr_log(log, L_DBG, file, line, "  Id       : %u", packet->data[1]);
       fr_log(log, L_DBG, file, line, "  Length   : %u", fr_nbo_to_uint16(packet->data + 2));
       fr_log(log, L_DBG, file, line, "  Vector   : %pH", fr_box_octets(packet->data + 4, RADIUS_AUTH_VECTOR_LENGTH));

       if (packet->data_len <= 20) return;

       for (attr = packet->data + 20, end = packet->data + packet->data_len;
            attr < end;
            attr += attr[1]) {
               int		i, len, offset = 2;
               unsigned int	vendor = 0;
	       char		*p;
	       char const	*truncated = "";

#ifndef NDEBUG
               if (attr[1] < 2) break; /* Coverity */
#endif

	       snprintf(buffer, sizeof(buffer), "%02x %02x  ", attr[0], attr[1]);
	       p = buffer + strlen(buffer);
               if ((attr[0] == FR_VENDOR_SPECIFIC) &&
                   (attr[1] > 6)) {
                       vendor = fr_nbo_to_uint32(attr + 2);

		       snprintf(p, buffer + sizeof(buffer) - p, "%02x%02x%02x%02x (%u)  ",
				attr[2], attr[3], attr[4], attr[5], vendor);
                       offset = 6;
		       p += strlen(p);
               }

	       len = attr[1] - offset;
	       if (len > 15) {
		       len = 15;
		       truncated = "...";
	       }

	       for (i = 0; i < len; i++) {
		       snprintf(p, buffer + sizeof(buffer) - p, "%02x ", attr[offset + i]);
		       p += 3;
	       }

	       fr_log(log, L_DBG, file, line, "      %s%s\n", buffer, truncated);
       }
}
