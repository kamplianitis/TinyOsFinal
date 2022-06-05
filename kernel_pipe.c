


#include "kernel_sched.h"
#include "kernel_cc.h"
#include "kernel_pipe.h"





//these are invalid functions that return -1 if a writer tries to read or a reader tries to write

int invalid_r(void* pipe,char* buf, uint size){
	return -1;
}
int invalid_w(void* pipe,const char* buf, uint size){
	return -1;
}




int pipe_read(void* pipe, char* buf, uint size){
	PICB* picb = (PICB*) pipe;

	uint count =  0;

//if we have not write, we cannot read so wait
	while(picb->readIndex == picb->writeIndex && picb->writer_done == 0){
		kernel_wait(&picb->buffer_empty, SCHED_PIPE);
	}

//if writer is done we take everything writen
	if(picb->writer_done == 1){
		while(picb->writeIndex != picb->readIndex){
			if((count == size) || (count == BUFFER_SIZE)){
				return count;
			}
			buf[count] = picb->buffer[picb->readIndex];
			picb->readIndex = (picb->readIndex + 1) % BUFFER_SIZE;
			count++;
		}
		return count;
	}

  while(1) {
		if(picb->readIndex == picb->writeIndex && picb->writer_done == 0){
			break;
		}
		if(count == size || count == BUFFER_SIZE){
			break;
		}
		buf[count] = picb->buffer[picb->readIndex];
		picb->readIndex = (picb->readIndex + 1) % BUFFER_SIZE;
		count++;
  }
	if(count == BUFFER_SIZE)return 0;

//broadcast to wake up writer
	kernel_broadcast(&picb->buffer_full);
	return count;
}




int pipe_write(void* pipe,const char* buf, uint size){

	PICB* picb = (PICB*)pipe;


	if(picb->reader_done == 1){
		return -1;
	}
//while reader has not read, we must wait
	while((picb->writeIndex + 1) % BUFFER_SIZE == picb->readIndex && picb->reader_done == 0){
		kernel_wait(&picb->buffer_full, SCHED_PIPE);
	}

	uint count = 0;
	if(picb->reader_done == 1) return -1;

	while(1){
			if((picb->writeIndex + 1) % BUFFER_SIZE == picb->readIndex && picb->reader_done == 0){
				break;
			}
			if(count == size || count == BUFFER_SIZE)break;

			picb->buffer[picb->writeIndex] = buf[count];
			picb->writeIndex = (picb->writeIndex + 1) % BUFFER_SIZE;
			count++;
	}

//broadcast to wake up reader
	kernel_broadcast(&picb->buffer_empty);
  return count;
}


int read_pipe_close(void* streamobj){


	PICB* picb = (PICB*)streamobj;

	if(picb == NULL) return -1;
	picb->reader_done = 1; //we close reader so reader_done=1
	kernel_broadcast(&picb->buffer_full);
	picb->ref--;  //decrease a ref count of the picb since we are closing the reading peer
		if(picb->writer_done == 1 && !picb->ref){ //if when we close reader, writer is already closed and reference counter=0, picb must be released
			free(picb);
			picb = NULL;
		}

		return 0;

}

int write_pipe_close(void* streamobj){

	PICB* picb = (PICB*)streamobj;

	if(picb == NULL) return -1;

	picb->writer_done = 1;  //we close writer so writer_done=1
	kernel_broadcast(&picb->buffer_empty);
	picb->ref--;  //decrease a ref count of the picb since we are closing the writing peer
	if(picb->reader_done == 1 && !picb->ref){//if when we close writter, reader is already closed and reference counter=0, picb must be released
		free(picb);
		picb = NULL;
	}

	return 0;
}

//file operations for read/write pipe are open=null because sys_pipe takes the lead
static file_ops read_pipe_operations = {
	.Open = NULL,
	.Read = pipe_read,
	.Write = invalid_w,
	.Close = read_pipe_close
};

static file_ops write_pipe_operations = {
	.Open = NULL,
	.Read = invalid_r,
	.Write = pipe_write,
	.Close = write_pipe_close
};




int sys_Pipe(pipe_t* pipe)
{
	Fid_t fid[2];
	FCB* fcb[2] = {NULL, NULL};

	FCB_reserve(2,&fid[0],&fcb[0]); //reservation of 2 fid's and 2 fcb's

	if(fcb[0] == NULL || fcb[1] == NULL){
		return -1;
	}


		PICB* picb = (PICB*) xmalloc(sizeof(PICB));

		picb->reader = fcb[0];    //get_fcb(pipe->read);
		picb->writer = fcb[1];    //get_fcb(pipe->write);


//initializations of every variable of picb
		picb->buffer_full = COND_INIT;
		picb->buffer_empty = COND_INIT;
		picb->reader_done = 0;
		picb->writer_done = 0;
		picb->ref = 0;
		pipe->read = fid[0];
		pipe->write = fid[1];

		picb->writeIndex = 0;
		picb->readIndex = 0;

	FCB* reader = picb->reader;
	FCB* writer = picb->writer;

	reader->streamobj = picb;
	writer->streamobj = picb;



	reader->streamfunc = &read_pipe_operations;
	writer->streamfunc = &write_pipe_operations;


	return 0;
}
