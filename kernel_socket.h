#include "kernel_pipe.h"
#include "kernel_streams.h"

//all posible types of sockets
typedef enum Socket_types{
  UNBOUND,
  LISTENER,
  PEER
}Socket_type;

typedef struct socket_control_block SCB;

//peer socket's special variables
typedef struct peer_socket_control_block{
    SCB* o_peer;
    PICB* send;
    PICB* recv;
}PSCB;


//listener socket's special variable
typedef struct listener_socket_control_block{
    rlnode request_queue;
    CondVar req_available;
}LSCB;

//declaration of socket control block 
struct socket_control_block{

    int ref_count;

    FCB* socket_fcb;

    port_t port;

    Socket_type s_type;

    union{
      LSCB listener;
      PSCB peer;
    };

};
