/*
    Copyright (c) 2007-2015 Contributors as noted in the AUTHORS file

    This file is part of libzmq, the ZeroMQ core engine in C++.

    libzmq is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    As a special exception, the Contributors give you permission to link
    this library with independent modules to produce an executable,
    regardless of the license terms of these independent modules, and to
    copy and distribute the resulting executable under terms of your choice,
    provided that you also meet, for each linked independent module, the
    terms and conditions of the license of that module. An independent
    module is a module which is not derived from or based on this library.
    If you modify this library, you must extend this exception to your
    version of the library.

    libzmq is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
    License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __ZMQ_YQUEUE_HPP_INCLUDED__
#define __ZMQ_YQUEUE_HPP_INCLUDED__

#include <stdlib.h>
#include <stddef.h>

#include "err.hpp"
#include "atomic_ptr.hpp"

namespace zmq
{

    //  yqueue is an efficient queue implementation. The main goal is
    //  to minimise number of allocations/deallocations needed. Thus yqueue
    //  allocates/deallocates elements in batches of N.
    //
    //  yqueue allows one thread to use push/back function and another one
    //  to use pop/front functions. However, user must ensure that there's no
    //  pop on the empty queue and that both threads don't access the same
    //  element in unsynchronised manner.
    //
    //  T is the type of the object in the queue.
    //  N is granularity of the queue (how many pushes have to be done till
    //  actual memory allocation is required).

    template <typename T, int N> class yqueue_t
    {
    public:

        //  Create the queue.
        inline yqueue_t ()
        {
             begin_chunk = (chunk_t*) malloc (sizeof (chunk_t));
             alloc_assert (begin_chunk);
             begin_pos = 0;
             back_chunk = NULL;
             back_pos = 0;
             end_chunk = begin_chunk; // end_chunk为back_chunk的先行者，之后会把最新的值交给back_trunk
             end_pos = 0;
        }

        //  Destroy the queue.
        inline ~yqueue_t ()
        {
            while (true) {
                if (begin_chunk == end_chunk) {
                    free (begin_chunk);
                    break;
                }
                chunk_t *o = begin_chunk;
                begin_chunk = begin_chunk->next;
                free (o);
            }

            chunk_t *sc = spare_chunk.xchg (NULL);
            free (sc);
        }

        //  Returns reference to the front element of the queue.
        //  If the queue is empty, behaviour is undefined.
        inline T &front ()
        {
             return begin_chunk->values [begin_pos];
        }

        //  Returns reference to the back element of the queue.
        //  If the queue is empty, behaviour is undefined.
        inline T &back ()
        {
            return back_chunk->values [back_pos];
        }

        //  Adds an element to the back end of the queue.
        inline void push ()
        {
            back_chunk = end_chunk;
            back_pos = end_pos;

            if (++end_pos != N)
                return;

            chunk_t *sc = spare_chunk.xchg (NULL); // 将spare_chunk置为NULL， 并返回spare_chunk原来的值
            
            if (sc) { //已经有过trunk使用记录的地址，复用以增加cache命中率
                end_chunk->next = sc;
                sc->prev = end_chunk;
            } else { // 开辟新的未使用过的trunk
                end_chunk->next = (chunk_t*) malloc (sizeof (chunk_t));
                alloc_assert (end_chunk->next);
                end_chunk->next->prev = end_chunk;
            }
            end_chunk = end_chunk->next;
            end_pos = 0;
        }

        //  Removes element from the back end of the queue. In other words
        //  it rollbacks last push to the queue. Take care: Caller is
        //  responsible for destroying the object being unpushed.
        //  The caller must also guarantee that the queue isn't empty when
        //  unpush is called. It cannot be done automatically as the read
        //  side of the queue can be managed by different, completely
        //  unsynchronised thread.
        inline void unpush ()
        {
            //  First, move 'back' one position backwards.
            if (back_pos)
                --back_pos;
            else {
                back_pos = N - 1;
                back_chunk = back_chunk->prev;
            }

            //  Now, move 'end' position backwards. Note that obsolete end chunk
            //  is not used as a spare chunk. The analysis shows that doing so
            //  would require free and atomic operation per chunk deallocated
            //  instead of a simple free.
            if (end_pos)
                --end_pos;
            else {
                end_pos = N - 1;
                end_chunk = end_chunk->prev;
                free (end_chunk->next);
                end_chunk->next = NULL;
            }
        }

        //  Removes an element from the front end of the queue.
        inline void pop ()
        {
            if (++ begin_pos == N) {
                chunk_t *o = begin_chunk;
                begin_chunk = begin_chunk->next;
                begin_chunk->prev = NULL;
                begin_pos = 0;

                //  'o' has been more recently used than spare_chunk,
                //  so for cache reasons we'll get rid of the spare and
                //  use 'o' as the spare.
                // spare_chunk的xchg方法类似跷跷板，将之前的值返回
                chunk_t *cs = spare_chunk.xchg (o); 
                // 如果之前的值为NULL，则free(NULL)无影响；
                // 否则回收由于连续pop产生的连续2个trunk内存块
                free (cs);
            }
        }

    private:

        //  Individual memory chunk to hold N elements.
        struct chunk_t
        {
             T values [N];
             chunk_t *prev;
             chunk_t *next;
        };

        //  Back position may point to invalid memory if the queue is empty,
        //  while begin & end positions are always valid. Begin position is
        //  accessed exclusively be queue reader (front/pop), while back and
        //  end positions are accessed exclusively by queue writer (back/push).
        chunk_t *begin_chunk;
        int begin_pos;
        chunk_t *back_chunk;
        int back_pos;
        chunk_t *end_chunk;
        int end_pos;

        //  People are likely to produce and consume at similar rates.  In
        //  this scenario holding onto the most recently freed chunk saves
        //  us from having to call malloc/free.
        atomic_ptr_t<chunk_t> spare_chunk;

        //  Disable copying of yqueue.
        yqueue_t (const yqueue_t&);
        const yqueue_t &operator = (const yqueue_t&);
    };

}

#endif
