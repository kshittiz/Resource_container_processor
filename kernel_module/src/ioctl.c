//////////////////////////////////////////////////////////////////////
//                      North Carolina State University
//
//
//
//                             Copyright 2016
//
////////////////////////////////////////////////////////////////////////
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
// This program is distributed in the hope it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
//
////////////////////////////////////////////////////////////////////////
//
//   Author:  Hung-Wei Tseng, Yu-Chia Liu
//
//   Description:
//     Core of Kernel Module for Processor Container
//
////////////////////////////////////////////////////////////////////////

#include "processor_container.h"

#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/kthread.h>


// Project 1: Kshittiz Kumar, kkumar4 (unity Id); Sujal Sujal, ssujal (unity Id); 

struct container {
__u64 cid;
struct container_thread* thread; //represent head of thread list
struct container* next;
struct mutex mylock; //each container will have its own lock, this improves efficiency over global lock mechanism
} *con_head = NULL;

struct container_thread {
pid_t pid;
struct task_struct* tsk;
struct container_thread* next;
};


/**
This function deletes container based on cid provided and free its memory
**/
void delete_container(__u64 cid) {
	struct container* temp = con_head;
	struct container* prev = con_head;
	while(temp) {
		if(temp->cid == cid) {
		  break;
		}
		prev = temp;
		temp = temp->next;
	}
	if(prev == temp) {
		con_head = temp->next;
		mutex_unlock(&temp->mylock);
		kfree(temp);
	} else {
		prev->next = temp->next;
		mutex_unlock(&temp->mylock);
		kfree(temp);
	}
	
}

/**
This function returns container associated with cid provided
**/
struct container* find_my_container(__u64 cid) {
	struct container* temp = con_head;
	while(temp) {
		if(temp->cid == cid) {
		  break;
		}
		temp = temp->next;
	}
	return temp;
}

/**
This function returns container associated with current task
**/
struct container* find_container_of_current_task(void) {
	struct container* temp = con_head;
	while(temp) {
		if(temp->thread->pid == current->pid) //thread found in this container at first position or anywhere after
			break;
		temp = temp->next;
	}
	return temp;
}

/**
 * Delete the task in the container.
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), 
 */
int processor_container_delete(struct processor_container_cmd __user *user_cmd)
{	
	struct container* myContainer;
	
	myContainer = find_container_of_current_task(); //finding correct container associated with this thread
	if(myContainer) { //container is not empty
		mutex_lock(&myContainer->mylock); //attaining lock

		if(myContainer->thread) {//container thread not empty
			if(myContainer->thread->pid == current->pid) { //trying to delete first thread
				struct container_thread* temp = myContainer->thread->next;
				struct container_thread* curr = myContainer->thread;

				if(temp) { //if not null 
					myContainer->thread = temp;
					wake_up_process(temp->tsk);
				} else {
					myContainer->thread = NULL;
				}

				kfree(curr);
			}  


			if(!myContainer->thread) { //if container becomes empty then delete it too
				delete_container(myContainer->cid); //lock will be released by this function
			} else {
				mutex_unlock(&myContainer->mylock);
			}

		} else {
			delete_container(myContainer->cid); //lock will be released by this function
		}
		
	}
	
    	return 0;
}

/**
 * Create a task in the corresponding container.
 * external functions needed:
 * copy_from_user(), mutex_lock(), mutex_unlock(), set_current_state(), schedule()
 * 
 * external variables needed:
 * struct task_struct* current  
 */
int processor_container_create(struct processor_container_cmd __user *user_cmd)
{
	struct container* myContainer;
	struct container_thread* myThread;
	struct  processor_container_cmd temp;
	
	copy_from_user(&temp, user_cmd, sizeof(struct processor_container_cmd));

	//if container head is not null, then finding current container
	if(con_head) {
		myContainer = find_my_container((&temp)->cid);
		if(!myContainer) { //container not found, create new
			struct container* temp_head = con_head;
			while(temp_head && temp_head->next)
				temp_head = temp_head->next;
			myContainer = (struct container*)kmalloc(sizeof(struct container), GFP_KERNEL);
			myContainer->cid = (&temp)->cid; 
			myContainer->next = NULL;
			myContainer->thread = NULL;
			mutex_init(&myContainer->mylock);
			temp_head->next = myContainer;
		}
	} else { //creating new container
		myContainer = (struct container*)kmalloc(sizeof(struct container), GFP_KERNEL);
		myContainer->cid = (&temp)->cid; 
		myContainer->next = NULL;
		myContainer->thread = NULL;
		mutex_init(&myContainer->mylock);
		con_head = myContainer;//initializing head
	}
	
	//creating new thread based on current task
	myThread = (struct container_thread*)kmalloc(sizeof(struct container_thread), GFP_KERNEL);
	myThread->pid = current->pid;
	myThread->tsk = current;
	myThread->next = NULL;
	
	mutex_lock(&myContainer->mylock);

	//if containers thread is not null
	if(myContainer->thread) {
		struct container_thread* temp_thread = myContainer->thread;
		while(temp_thread && temp_thread->next)
			temp_thread = temp_thread->next;
		temp_thread->next = myThread;

		mutex_unlock(&myContainer->mylock); //unlocking before sleep
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	} else {
		myContainer->thread = myThread;
		mutex_unlock(&myContainer->mylock);
	}

	return 0;
}

/**
 * switch to the next task in the next container
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), set_current_state(), schedule()
 */
int processor_container_switch(struct processor_container_cmd __user *user_cmd)
{
	struct container* myContainer;

	myContainer = find_container_of_current_task(); //finding correct container associated with this current thread
	if(myContainer) { //container is not empty
		mutex_lock(&myContainer->mylock);

		struct container_thread* top = myContainer->thread; //holding top of thread

		if(top && top->next) { //if there is some next task in this container switch to that
			myContainer->thread = top->next; //moving to next next task;
			struct container_thread* temp_thread = myContainer->thread; //lets start with next thread
			top->next = NULL; //making first task point to nothing				
			while(temp_thread && temp_thread->next) //reaching end of the list
				temp_thread = temp_thread->next;
			
			temp_thread->next = top; //adding first task at the end;

			wake_up_process(myContainer->thread->tsk);//waking up next task which is already place at top of the list
			mutex_unlock(&myContainer->mylock); //unlocking before sleep

			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
		} else {
			mutex_unlock(&myContainer->mylock); //unlocking before sleep
		}
	} 
    return 0;
}


/**
 * control function that receive the command in user space and pass arguments to
 * corresponding functions.
 */
int processor_container_ioctl(struct file *filp, unsigned int cmd,
                              unsigned long arg)
{
    switch (cmd)
    {
    case PCONTAINER_IOCTL_CSWITCH:
        return processor_container_switch((void __user *)arg);
    case PCONTAINER_IOCTL_CREATE:
        return processor_container_create((void __user *)arg);
    case PCONTAINER_IOCTL_DELETE:
        return processor_container_delete((void __user *)arg);
    default:
        return -ENOTTY;
    }
}
