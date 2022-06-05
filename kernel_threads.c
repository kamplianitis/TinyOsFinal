
#include <assert.h>

#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_streams.h"



void start_thread(){
  int exitval;
  Task call = CURTHREAD->ptcb->task;
  int argl = CURTHREAD->ptcb->argl;
  void* args = CURTHREAD->ptcb->args;
  exitval = call(argl,args);

  ThreadExit(exitval);

}

/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
  //Cache the current process
  PCB* curproc = CURPROC;


  //Allocate a new process thread 
  PTCB* new_ptcb = (PTCB*)xmalloc(sizeof(PTCB)); 

  //Initialize rlnode of the new PTCB  to point to itself
  rlnode_init(&new_ptcb->thread_list_node, new_ptcb);

  //Initialize ptcb values
  new_ptcb->exited = 0;
  new_ptcb->detached = 0;
  new_ptcb->ref_count = 0;
  new_ptcb->exit_cv = COND_INIT;
  

  //Pass arguments to the new process thread
  new_ptcb->task = task; 
  new_ptcb->argl = argl;
  new_ptcb->args = args;
  

  
  //Add allocated process thread to the thread list of the current process
  rlist_push_back(&curproc->thread_list, &new_ptcb->thread_list_node);

  //Increase thread_count 
  curproc->thread_count++;
  

  //Spawn a new thread and add it to the new ptcb 
  new_ptcb->tcb = spawn_thread(curproc,start_thread);
  new_ptcb->tcb->ptcb = new_ptcb;

  //Add the new thread to the scheduler queue
  wakeup(new_ptcb->tcb);

  //Return the new thread
	return (Tid_t) new_ptcb;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	return (Tid_t) CURTHREAD;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{

  PTCB* ptcb = (PTCB*)tid;
  assert(ptcb != NULL);
  /*  Return error -1 if the thread to be joined is:
      detached,
      the current thread,
      not in the thread list of the current process
  */

  if(ptcb->detached == 1 || ptcb->tcb == CURTHREAD || rlist_find(&CURPROC->thread_list,ptcb,NULL) != &ptcb->thread_list_node){
    return -1;
  }

//If the thread to be joined is already exited, return its exitval
  if(ptcb->exited == 1){
    return *exitval;
  }

  //Increase the ref_count of the joined ptcb
    ptcb->ref_count++;

  //If the joined thread is not exited, wait for it to exit
  while(ptcb->exited == 0 && ptcb->detached == 0){
    kernel_wait(&ptcb->exit_cv,SCHED_USER);
  }
  /*
    Now, the joined thread has exited and I need not wait for it anymore
  */

  //Decrease the ref_count of the joined ptcb
  if(ptcb->ref_count > 0){
    ptcb->ref_count--;
  }

  //If the ref_count falls to zero,no thread is waiting for this thread to exit, so it can be released
  if((ptcb->ref_count == 0 && rlist_find(&CURPROC->thread_list,&ptcb->thread_list_node,NULL) == &ptcb->thread_list_node && ptcb->detached == 0 )|| ptcb->detached == 1 ){
    rlist_remove(&ptcb->thread_list_node);
    free(ptcb);
    //Decrease the thread_count of the current process, since one of its threads was removed from its thread_list
    CURPROC->thread_count--;
  }

  return 0;
}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid)
{
  PTCB* ptcb = (PTCB*) tid;
  /*
  
    Mark the ptcb as detached and broadcast a signal, so that threads that have joined this ptcb cease to wait.

  */
  if(ptcb->detached == 0){
    ptcb->detached = 1;
    kernel_broadcast(&ptcb->exit_cv);
    return 0;
  }

	return -1;
}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{
  
  PTCB* current_ptcb = CURTHREAD->ptcb;
  PCB *curproc = CURPROC;  /* cache for efficiency */

  //Update the values of the ptcb
  current_ptcb->exitval = exitval;
  current_ptcb->exited = 1;
  current_ptcb->args = NULL;
  
  //Broadcast exit signal
  kernel_broadcast(&current_ptcb->exit_cv); 

  //Remove the ptcb from the thread list and free it
  if(current_ptcb->detached == 1){
   rlist_remove(&current_ptcb->thread_list_node);
    free(current_ptcb); 
  }
  if(current_ptcb->ref_count == 0 && current_ptcb->detached == 0){
    rlist_remove(&current_ptcb->thread_list_node);
    free(current_ptcb);
  }
  
  //Decrease thread_count of the current procees
  curproc->thread_count--;


  /* If the thread_count becomes zero, all threads are exited and the process must terminate. */
  if(curproc->thread_count == 0){

       curproc->exitval = exitval;
        

      /* Do all the other cleanup we want here, close files etc. */
      if(curproc->args) {
        free(curproc->args);
        curproc->args = NULL;
      }

      /* Clean up FIDT */
      for(int i=0;i<MAX_FILEID;i++) {
        if(curproc->FIDT[i] != NULL) {
          FCB_decref(curproc->FIDT[i]);
          curproc->FIDT[i] = NULL;
        }
      }

      /* Reparent any children of the exiting process to the 
         initial task */
      PCB* initpcb = get_pcb(1);
      while(!is_rlist_empty(& curproc->children_list)) {
        rlnode* child = rlist_pop_front(& curproc->children_list);
        child->pcb->parent = initpcb;
        rlist_push_front(& initpcb->children_list, child);
      }

      /* Add exited children to the initial task's exited list 
         and signal the initial task */
      if(!is_rlist_empty(& curproc->exited_list)) {
        rlist_append(& initpcb->exited_list, &curproc->exited_list);
        kernel_broadcast(& initpcb->child_exit);
      }

      /* Put me into my parent's exited list */
      if(curproc->parent != NULL) {   /* Maybe this is init */
        rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
        kernel_broadcast(& curproc->parent->child_exit);
      }
      /* Now, mark the process as exited. */
      curproc->pstate = ZOMBIE;
  }
  
  /*The thread is about to become history...*/
  kernel_sleep(EXITED,SCHED_USER);

  

}

