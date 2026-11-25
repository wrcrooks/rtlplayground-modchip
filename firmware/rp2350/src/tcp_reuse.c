#include "lwip/priv/tcp_priv.h"

/* RFC 1122 4.2.2.13: a newer SYN reopens a connection in TIME_WAIT. The old
 * one goes here, the SYN when it is sent again, a second later. */
err_t net_tcp_in(struct tcp_pcb *pcb, const struct tcp_hdr *h)
{
	if (pcb->state != TIME_WAIT || (TCPH_FLAGS(h) & (TCP_SYN | TCP_ACK | TCP_RST)) != TCP_SYN
	    || !TCP_SEQ_GT(h->seqno, pcb->rcv_nxt))
		return ERR_OK;
	tcp_abort(pcb);
	return ERR_ABRT;
}
