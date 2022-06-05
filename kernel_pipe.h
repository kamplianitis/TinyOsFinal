#include "kernel_streams.h"
#include "tinyos.h"

#define BUFFER_SIZE 8192

//declaration of the functions we use in kernel_pipe.c
int invalid_r(void* pipe,char* buf, uint size);
int invalid_w(void* pipe,const char* buf, uint size);
int pipe_read(void* pipe, char* buf, uint size);
int pipe_write(void* pipe,const char* buf, uint size);
int read_pipe_close(void* streamobj);
int write_pipe_close(void* streamobj);

//declaration of pipe control block
typedef struct pipe_control_block{
	char buffer[BUFFER_SIZE];


	FCB* reader;
	FCB* writer;

	int ref;

	int reader_done;
	int writer_done;

	CondVar buffer_full;
	CondVar buffer_empty;

	int writeIndex;
	int readIndex;


}PICB;
