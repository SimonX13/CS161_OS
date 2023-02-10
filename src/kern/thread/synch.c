/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Synchronization primitives.
 * The specifications of the functions are in synch.h.
 */

#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <wchan.h>
#include <thread.h>
#include <current.h>
#include <synch.h>

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *name, unsigned initial_count)
{
        struct semaphore *sem;

        sem = kmalloc(sizeof(struct semaphore));
        if (sem == NULL) {
                return NULL;
        }

        sem->sem_name = kstrdup(name);
        if (sem->sem_name == NULL) {
                kfree(sem);
                return NULL;
        }

	sem->sem_wchan = wchan_create(sem->sem_name);
	if (sem->sem_wchan == NULL) {
		kfree(sem->sem_name);
		kfree(sem);
		return NULL;
	}

	spinlock_init(&sem->sem_lock);
        sem->sem_count = initial_count;

        return sem;
}

void
sem_destroy(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	/* wchan_cleanup will assert if anyone's waiting on it */
	spinlock_cleanup(&sem->sem_lock);
	wchan_destroy(sem->sem_wchan);
        kfree(sem->sem_name);
        kfree(sem);
}

void
P(struct semaphore *sem)
{
        KASSERT(sem != NULL);

        /*
         * May not block in an interrupt handler.
         *
         * For robustness, always check, even if we can actually
         * complete the P without blocking.
         */
        KASSERT(curthread->t_in_interrupt == false);

	/* Use the semaphore spinlock to protect the wchan as well. */
	spinlock_acquire(&sem->sem_lock);
        while (sem->sem_count == 0) {
		/*
		 *
		 * Note that we don't maintain strict FIFO ordering of
		 * threads going through the semaphore; that is, we
		 * might "get" it on the first try even if other
		 * threads are waiting. Apparently according to some
		 * textbooks semaphores must for some reason have
		 * strict ordering. Too bad. :-)
		 *
		 * Exercise: how would you implement strict FIFO
		 * ordering?
		 */
		wchan_sleep(sem->sem_wchan, &sem->sem_lock);
        }
        KASSERT(sem->sem_count > 0);
        sem->sem_count--;
	spinlock_release(&sem->sem_lock);
}

void
V(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	spinlock_acquire(&sem->sem_lock);

        sem->sem_count++;
        KASSERT(sem->sem_count > 0);
	wchan_wakeone(sem->sem_wchan, &sem->sem_lock);

	spinlock_release(&sem->sem_lock);
}

////////////////////////////////////////////////////////////
//
// Lock.

struct lock *
lock_create(const char *name)
{
        struct lock *lock;

        lock = kmalloc(sizeof(struct lock)); //free space for lock
        if (lock == NULL) {
                return NULL;
        }

        lock->lk_name = kstrdup(name); //name the lock
        if (lock->lk_name == NULL) {
                kfree(lock); 
                return NULL;
        }

        lock->lock_wchan = wchan_create(lock->lk_name); //creates a wait channel for the thread to wait in while sleeping
        if (lock->lock_wchan == NULL){
                kfree(lock->lk_name);
                kfree(lock);
                return NULL;
        }

        spinlock_init(&lock->lock_splock); //creates spinlock to prevent race conditions
        lock->acquired = false; //declares state of lock to vacant 
        lock->current_thread = NULL; //declares that no thread currently occupies the lock
        
        KASSERT(lock->current_thread == NULL); //ensures that the lock is vacant

        return lock;
}

void
lock_destroy(struct lock *lock)
{
        KASSERT(lock != NULL); //ensures the lock exists
        KASSERT(lock->current_thread == NULL); //ensures that lock isn't acquired by any thread

        spinlock_cleanup(&lock->lock_splock); //clears space for spinlock
        wchan_destroy(lock->lock_wchan); //clears space occupied by wait channel
        kfree(lock->lk_name); 
        kfree(lock);
}

void
lock_acquire(struct lock *lock)
{

        KASSERT(lock != NULL);
        KASSERT(curthread->t_in_interrupt == false); //check if we can acquire without blocking 

        spinlock_acquire(&lock->lock_splock); //acquires spinlock to lock wait channel to prevent race conditions

        while (lock->acquired){                           
                wchan_sleep(lock->lock_wchan, &lock->lock_splock); //if lock is under use, send thread to wait channel to sleep
        }  
        
        KASSERT(!lock->acquired); //make sure lock is still available

        lock->acquired = true; //acquire lock
        lock->current_thread = curthread; //update thread info in lock
        spinlock_release(&lock->lock_splock);

}

void
lock_release(struct lock *lock)
{
        KASSERT(lock != NULL); 
        KASSERT(curthread->t_in_interrupt == false);
        KASSERT(lock->acquired); //ensures the lock is not already free

        spinlock_acquire(&lock->lock_splock); 

        lock->acquired = false; //changes state of lock to vacant
        lock->current_thread = NULL; //deletes thread information 

        KASSERT(!lock->acquired); //ensures lock is now free

        wchan_wakeone(lock->lock_wchan, &lock->lock_splock); //wake up the thread from wait channel
        spinlock_release(&lock->lock_splock); //release lock 

}

bool
lock_do_i_hold(struct lock *lock)
{

        KASSERT(lock != NULL);

        if (lock->current_thread == curthread) return true; //checks current condition of the lock 
        else if (lock->acquired == false) return false;
        else return false;

}

////////////////////////////////////////////////////////////
//
// CV


struct cv *
cv_create(const char *name)
{
        struct cv *cv;

        cv = kmalloc(sizeof(struct cv)); //allocate space for cv
        if (cv == NULL) {
                return NULL;
        }

        cv->cv_name = kstrdup(name); //name the cv 
        if (cv->cv_name==NULL) {
                kfree(cv);
                return NULL;
        }

        spinlock_init(&cv->cv_splock); //initiate spinlock for the same reason as above

        cv->cv_wchan = wchan_create(cv->cv_name); //create wait channel for while therads don't meet conditions
        if (cv->cv_wchan == NULL){
                kfree(cv->cv_name);
                kfree(cv);
                return NULL;
        }

        return cv;
}

void
cv_destroy(struct cv *cv)
{
        KASSERT(cv != NULL);

        //clean up the created spaces

        spinlock_cleanup(&cv->cv_splock);
        wchan_destroy(cv->cv_wchan);
        kfree(cv->cv_name);
        kfree(cv);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
        KASSERT(cv != NULL); 
        KASSERT(lock != NULL);
        KASSERT(curthread->t_in_interrupt == false);

        spinlock_acquire(&cv->cv_splock); //needs spinlock same reason as above, to ensure that only one thread is processed at a time and prevents race condition
        lock_release(lock); //thread doesn't meet condition so it must go wait in the wait channel. Before that it needs to release the lock so other threads cna acquire it
        wchan_sleep(cv->cv_wchan, &cv->cv_splock); //puts the thread to sleep

        spinlock_release(&cv->cv_splock);

        lock_acquire(lock); //after getting out of wake channel, thread now meets condition hence can acquire lock again
        

}

void
cv_signal(struct cv *cv, struct lock *lock)
{
        KASSERT(cv != NULL);
        KASSERT(lock != NULL);
        KASSERT(curthread->t_in_interrupt == false);
        
        spinlock_acquire(&cv->cv_splock);

        wchan_wakeone(cv->cv_wchan, &cv->cv_splock); //wakes only one thread sleeping in the wait channel

        spinlock_release(&cv->cv_splock);
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
        KASSERT(cv != NULL); 
        KASSERT(lock != NULL);
        KASSERT(curthread->t_in_interrupt == false);
        
        spinlock_acquire(&cv->cv_splock);

        wchan_wakeall(cv->cv_wchan, &cv->cv_splock); //wakes all the threads sleeping in the wait channel

        spinlock_release(&cv->cv_splock);
}
