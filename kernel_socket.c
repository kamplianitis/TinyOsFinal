#include "kernel_socket.h"
#include "tinyos.h"
#include "util.h"
#include "kernel_sched.h"
#include "kernel_cc.h"
#include <assert.h>

SCB* PORT_MAP[MAX_PORT] = {[0 ... MAX_PORT-1] = NULL};
Mutex socket_spinlock = MUTEX_INIT;


//this is the request connect function makes
typedef struct request_block{
	SCB* requester;
	CondVar req_cv;
	int accepted;
	rlnode req_node;
}request;


int socket_read(void* this,char* buf,uint size){
	SCB* scb = (SCB*)this;

	if(scb != NULL && scb->s_type == PEER){ //if socket is a peer socket
		PICB* picb =(PICB*)scb->peer.recv;
		if(picb->reader_done)return -1;
		int rc = pipe_read(picb,buf,size); //calling pipe read
		return rc;
	}

	return -1;
}

int socket_write(void* this,const char* buf,uint size){
	SCB* scb = (SCB*)this;

	if(scb != NULL && scb->s_type == PEER){ //if socket is a peer socket
		PICB* picb =(PICB*)scb->peer.send;
		if(picb->writer_done)return -1;
		int wc = pipe_write(picb,buf,size); //calling pipe write
		return wc;
	}

	return -1;
}

//function to decrease ref_count and check if it is equal to zero to free the Socket
void decrease_refcount(SCB* scb){
	scb->ref_count--;
	if(!scb->ref_count && scb != NULL){
		free(scb);
		scb = NULL;
	}
}


int	socket_close(void* sock){


		SCB* socket = (SCB*)sock;
		int r = 0;
		int w = 0;
		request* req;
		if(socket == NULL){
			return -1;
		}
		switch(socket->s_type){
			case UNBOUND:
				decrease_refcount(socket);
				return 0;
			case LISTENER:
				PORT_MAP[socket->port] = NULL;
				while(!is_rlist_empty(&socket->listener.request_queue)){
						req = (request*)rlist_pop_front(&socket->listener.request_queue)->obj;
						req->accepted = 0;
						kernel_broadcast(&req->req_cv);
				}
				kernel_broadcast(&socket->listener.req_available);
				decrease_refcount(socket);
				return 0;
			case PEER:
			if(socket->peer.o_peer != NULL){
				r = read_pipe_close(socket->peer.recv);
				w = write_pipe_close(socket->peer.send);
				decrease_refcount(socket->peer.o_peer);
			}
				if(r+w == 0){
					return 0;
				} else return -1;
		};
		assert(0);
		return -1;
}



static file_ops socket_fops = {
	.Open = NULL,
	.Read = socket_read,
	.Write = socket_write,
	.Close = socket_close
};



Fid_t sys_Socket(port_t port){
	if(port < NOPORT || port > MAX_PORT){
		return NOFILE;
	}

	SCB* socket = (SCB*)xmalloc(sizeof(SCB));

	Fid_t fid[1];
	FCB* fcb[1];
	int f = FCB_reserve(1,&fid[0],&fcb[0]);
	if(f == 0){
		return NOFILE;
	}

	socket->ref_count = 1;
	socket->socket_fcb = fcb[0];
	socket->socket_fcb->streamobj = socket;
	socket->socket_fcb->streamfunc = &socket_fops;

	socket->port = port;

	socket->s_type = UNBOUND;
	return fid[0];
}



int sys_Listen(Fid_t sock)
{

	if(sock < 0 || sock >= MAX_FILEID){
		return -1;
	}

	FCB* fcb = get_fcb(sock);
	if(fcb == NULL || fcb->streamfunc != &socket_fops){
		return -1;
	}

	SCB* scb = (SCB*)fcb->streamobj;
	if(scb == NULL) return -1;

	if(scb->s_type == PEER || scb->s_type == LISTENER || PORT_MAP[scb->port] != NULL || scb->port == NOPORT){
		return -1;
	}

	scb->s_type = LISTENER;
	PORT_MAP[scb->port] = scb;

	rlnode_init(&scb->listener.request_queue,NULL);
	scb->listener.req_available = COND_INIT;


	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{
	if(lsock < 0 || lsock >= MAX_FILEID){
		return NOFILE;
	}


	//...............listener
	FCB* lsocket_fcb = get_fcb(lsock);
	if(lsocket_fcb == NULL || lsocket_fcb->streamfunc != &socket_fops){
		return NOFILE;
	}


	SCB* lsocket = (SCB*)lsocket_fcb->streamobj;

	if(lsocket == NULL){
		return NOFILE;
	}

	if(lsocket->s_type != LISTENER){
		return NOFILE;
	}

	lsocket->ref_count++;		if(PORT_MAP[lsocket->port] == NULL) return -1; //manos change

	while(is_rlist_empty(&lsocket->listener.request_queue)){
		kernel_wait(&lsocket->listener.req_available,SCHED_PIPE);
	}



	if(PORT_MAP[lsocket->port] != lsocket || PORT_MAP[lsocket->port] == NULL) return -1;


	Fid_t peerfid = sys_Socket(lsocket->port);
	if(peerfid == NOFILE)return -1;

	//...............peer the listener created
	SCB* lis_peer = get_fcb(peerfid)->streamobj;


	//..............request
	request* new_req =(request*) rlist_pop_back(&lsocket->listener.request_queue)->obj;


	//..............requester/peer
	SCB* req_peer = new_req->requester;

	lis_peer->s_type = PEER;
	req_peer->s_type = PEER;
	new_req->accepted = 1;


	PICB* pipe1 = (PICB*)xmalloc(sizeof(PICB));
	PICB* pipe2 = (PICB*)xmalloc(sizeof(PICB));

	//make pipe 1
	//pipe1->buffer = (char*)xmalloc(BUFFER_SIZE);
	pipe1->buffer_full = COND_INIT;
	pipe1->buffer_empty = COND_INIT;
	pipe1->writeIndex = 0;
	pipe1->readIndex = 0;
	pipe1->reader_done = 0;
	pipe1->writer_done = 0;
	pipe1->ref = 0;
	//make pipe2
	//pipe2->buffer = (char*)xmalloc(BUFFER_SIZE);
	pipe2->buffer_full = COND_INIT;
	pipe2->buffer_empty = COND_INIT;
	pipe2->writeIndex = 0;
	pipe2->readIndex = 0;
	pipe2->reader_done = 0;
	pipe2->writer_done = 0;
	pipe2->ref = 0;
	//connections
	lis_peer->peer.send = pipe1;
	lis_peer->peer.recv = pipe2;
	req_peer->peer.send = pipe2;
	req_peer->peer.recv = pipe1;
	lis_peer->peer.o_peer = req_peer;
	req_peer->peer.o_peer = lis_peer;
	pipe1->reader = lis_peer->socket_fcb;
	pipe1->writer = req_peer->socket_fcb;
	pipe2->reader = req_peer->socket_fcb;
	pipe2->writer = lis_peer->socket_fcb;
	pipe1->ref++;
	pipe2->ref++;

	decrease_refcount(lsocket);

	kernel_broadcast(&new_req->req_cv);
	return peerfid;
}



int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	if(sock < 0 || sock >= MAX_FILEID || port < 0 || port >= MAX_PORT){
		return -1;
	}

//take the listener socket from port map list
	SCB* lsock = PORT_MAP[port];


	if(lsock == NULL){
		return -1;
	}


	FCB* peer_fcb = get_fcb(sock);
	if(peer_fcb == NULL || peer_fcb->streamfunc != &socket_fops){
		return -1;
	}
	SCB* peer = peer_fcb->streamobj;
	if(peer->s_type != UNBOUND)return -1;

//***********************************************************request, the making of
	request* req = (request*) xmalloc(sizeof(request));
	rlnode_init(&req->req_node,req);
	req->requester = peer;
	req->req_cv = COND_INIT;
	req->accepted = 0;
	rlist_push_front(&lsock->listener.request_queue,&req->req_node);
//*************************************************************



	kernel_broadcast(&lsock->listener.req_available);

	int wait;

	while(!req->accepted){
		wait = kernel_timedwait(&req->req_cv,SCHED_PIPE,timeout); //waits on request cv with a timeout limit

		if(!wait || !req->accepted){
			return -1;
		}
		free(req);
		req = NULL;
		return 0;

	}
		assert(0);
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{

	SCB* scb = get_fcb(sock)->streamobj;
	if(scb->s_type != PEER || scb == NULL){
		return -1;
	}else{

	switch(how){
		case 1://this is for reader
			return read_pipe_close(scb->peer.recv);
		break;
		case 2://this is for writter
			return write_pipe_close(scb->peer.send);
		break;
		case 3://this is for both
			if(!read_pipe_close(scb->peer.recv) && !write_pipe_close(scb->peer.send)){
			 	return 0;
			}
				break;


	};
}
return -1;
}
