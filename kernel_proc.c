
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"



/*
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;
  pcb->thread_count = 0;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;
  rlnode_init(&pcb->thread_list, NULL);
  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  pcb->child_exit = COND_INIT;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb->thread_count = 0; //we initialize the amount of the threads into zero
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
	This function is provided as an argument to spawn,
	to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);
  Exit(exitval);
}


/*
	System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;

  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process)
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args != NULL){
  	newproc->args = malloc(argl);
  	memcpy(newproc->args,args,argl);
  }
  else{
  	newproc->args = NULL;
  }


  /*
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */

  if(call != NULL) {

  	//Allocate a new ptcb
  	PTCB* main_ptcb=xmalloc(sizeof(PTCB));

  	//Add the new ptcb to the thread list of the new process
	rlnode_init(&main_ptcb->thread_list_node,main_ptcb);
  	rlist_push_back(&newproc->thread_list,&main_ptcb->thread_list_node);


  	//Initialize the values of the ptcb
  	main_ptcb->exited = 0;
  	main_ptcb->detached = 0;
  	main_ptcb->ref_count = 1;
  	main_ptcb->exit_cv = COND_INIT;


  	//Increase the thread counter of the new process
	   newproc->thread_count++;

  	//Spawn a thread...
    main_ptcb->tcb = spawn_thread(newproc, start_main_thread);

    //...and add it to the ptcb
    main_ptcb->tcb->ptcb = main_ptcb;

  	//Pass arguments to the new process thread
  	main_ptcb->task = (void*)call;
  	main_ptcb->argl = argl;
  	main_ptcb->args = args;

  	//Add the new allocated thread to the scheduler queue
    wakeup(main_ptcb->tcb);
  }



finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);

  cleanup_zombie(child, status);

finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  if(is_rlist_empty(& parent->children_list)) {
    cpid = NOPROC;
    goto finish;
  }

  while(is_rlist_empty(& parent->exited_list)) {
    kernel_wait(& parent->child_exit, SCHED_USER);
  }

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

finish:
  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{
  /* Right here, we must check that we are not the boot task. If we are,
     we must wait until all processes exit. */
  if(sys_GetPid()==1) {
    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
  }

 	PCB* curproc = CURPROC;  /* cache for efficiency */

  	//Update the exitval of the process
    curproc->exitval = exitval;

  	//Calling exit function to release the allocated ptcb and potentionally terminate the proccess
    sys_ThreadExit(exitval);

}


// the user can never write so every time the function will return -1
int InfoWrite(void* proc_info,const char* buf, uint size)
{
	return -1;
}

int InfoRead(void* proc_info, char* buf, uint size)
{
    // take the proccb from the proc_info var
    // it will help me keep int track of my place in the process table
    // and i will have the proc_info to save all the needed info
    PROCCB* procInfo = (PROCCB*) proc_info;
    int counter = procInfo->cursor; // give the value of the cursor to a counter
    // so i can use it easier


    // if this is true that means that i am at the end of the procedure
    // has ended cause we are at the end of the process table
    if(counter == MAX_PROC)
    {
      return 0;
    }

    //loop to find a non empty pcb
    while(counter < MAX_PROC && counter>=0)
    {
      if((&PT[counter])->pstate != FREE) // that means i have found a non empty pcb
      {
        // have to transfer all the values to the procinfo that i have
        //////////////////////////////////////////////////////////

        PCB* cupcb = &PT[counter];
        // take all the values i need
        procInfo->cur_info.pid = counter;
        procInfo->cur_info.ppid = get_pid(cupcb->parent);
        ////////////////////////////////////////////////
        if(cupcb->pstate == ZOMBIE)
          procInfo->cur_info.alive = 0;         // declare the state of the pcb
        else
          procInfo->cur_info.alive = 1;
        //////////////////////////////////////////////

        // take the remaining values that i need
        procInfo->cur_info.thread_count = (unsigned long)cupcb->thread_count;
        procInfo->cur_info.main_task = cupcb->main_task;
        procInfo->cur_info.argl = cupcb->argl;
        // use the memcpy to take the args properly
        memcpy(procInfo->cur_info.args,(char *) &cupcb->args,PROCINFO_MAX_ARGS_SIZE);
        // same here so that i can pass to the buf all the asking info about the process
        memcpy(buf, (char*) &procInfo->cur_info, size);
        //increase the counter there so we begin from that point of the table next time
        // this helps us avoid taking the same process over and over
        counter++;
        // we found the first non-empty process so we dont need to keep the loop running
        break;
      }
      else
        // case the pcb that i am looking right now is empty i have to keep looking for
        // a non-empty one by increasing the counter
        counter++;
    }
    //last we have to put the value of the counter back to the cursor so we can keep the
    // position of the last non-empty process
    procInfo->cursor = counter;
    //the only way to reach that point is to go out of the loop so in that case we have
    // a non-empty pcb and we have to return the size of the char* buffer
    return size;
}


int InfoClose(void* streamobj)
{
  // take the proccb that we want to close
	PROCCB* proccb = (PROCCB*) streamobj;

  // we have to make it null and release its pointer
	if(proccb != NULL)
	{
      proccb = NULL;
    	free(proccb);
      return 0;
	}
  //case something is wrong and the proccb is closed already
	return -1;
}

static file_ops proc_info_operations =
{
  .Open = NULL,
  .Read = InfoRead,
  .Write = InfoWrite,
  .Close = InfoClose
};

Fid_t sys_OpenInfo()
{
  Fid_t fid;
  FCB* fcb = {NULL};


  FCB_reserve(1, &fid, &fcb);
  // creation of the process_control_block that we need
  PROCCB* proccb = (PROCCB*)xmalloc(sizeof(PROCCB));
  // creation of the procinfo that will connect to the proccb that we created earlier
  procinfo* proc_info = (procinfo*)xmalloc(sizeof(procinfo));

  //make the initializations of the proccb and the connection between it and the procinfo
  proccb->cur_info = *proc_info ;
  proccb->cursor = 0;


  // now i have to fill the fcbs with streamfunc
  fcb->streamfunc = &proc_info_operations;

  // next move is to fill the fcb with the right object-device
  fcb->streamobj = proccb; // i can keep the pointer that way

  //at last we return  the fid_t pointer
  return fid;
}
