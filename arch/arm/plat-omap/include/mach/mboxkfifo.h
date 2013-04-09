/*
 * A generic kernel FIFO implementation.
 *
 * Copyright (C) 2009 Stefani Seibold <stefani@seibold.net>
 * Copyright (C) 2004 Stelian Pop <stelian@popies.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

/*
 * Howto porting drivers to the new generic fifo API:
 *
 * - Modify the declaration of the "struct mbox_kfifo *" object into a
 *   in-place "struct mbox_kfifo" object
 * - Init the in-place object with mbox_kfifo_alloc() or mbox_kfifo_init()
 *   Note: The address of the in-place "struct mbox_kfifo" object must be
 *   passed as the first argument to this functions
 * - Replace the use of __mbox_kfifo_put into mbox_kfifo_in and __mbox_kfifo_get
 *   into mbox_kfifo_out
 * - Replace the use of mbox_kfifo_put into mbox_kfifo_in_locked and mbox_kfifo_get
 *   into mbox_kfifo_out_locked
 *   Note: the spinlock pointer formerly passed to mbox_kfifo_init/mbox_kfifo_alloc
 *   must be passed now to the mbox_kfifo_in_locked and mbox_kfifo_out_locked
 *   as the last parameter.
 * - All formerly name __mbox_kfifo_* functions has been renamed into mbox_kfifo_*
 */

#ifndef _LINUX_MBOX_KFIFO_H
#define _LINUX_MBOX_KFIFO_H

#include <linux/kernel.h>
#include <linux/spinlock.h>

struct mbox_kfifo {
        unsigned char *buffer;  /* the buffer holding the data */
        unsigned int size;      /* the size of the allocated buffer */
        unsigned int in;        /* data is added at offset (in % size) */
        unsigned int out;       /* data is extracted from off. (out % size) */
};

/*
 * Macros for declaration and initialization of the mbox_kfifo datatype
 */

/* helper macro */
#define __mbox_kfifo_initializer(s, b) \
        (struct mbox_kfifo) { \
                .size   = s, \
                .in     = 0, \
                .out    = 0, \
                .buffer = b \
        }

/**
 * DECLARE_MBOX_KFIFO - macro to declare a mbox_kfifo and the associated buffer
 * @name: name of the declared mbox_kfifo datatype
 * @size: size of the fifo buffer. Must be a power of two.
 *
 * Note1: the macro can be used inside struct or union declaration
 * Note2: the macro creates two objects:
 *  A mbox_kfifo object with the given name and a buffer for the mbox_kfifo
 *  object named name##mbox_kfifo_buffer
 */
#define DECLARE_MBOX_KFIFO(name, size) \
union { \
        struct mbox_kfifo name; \
        unsigned char name##mbox_kfifo_buffer[size + sizeof(struct mbox_kfifo)]; \
}

/**
 * INIT_MBOX_KFIFO - Initialize a mbox_kfifo declared by DECLARE_mbox_kfifo
 * @name: name of the declared mbox_kfifo datatype
 */
#define INIT_MBOX_KFIFO(name) \
        name = __mbox_kfifo_initializer(sizeof(name##mbox_kfifo_buffer) - \
                                sizeof(struct mbox_kfifo), \
                                name##mbox_kfifo_buffer + sizeof(struct mbox_kfifo))

/**
 * DEFINE_MBOX_KFIFO - macro to define and initialize a mbox_kfifo
 * @name: name of the declared mbox_kfifo datatype
 * @size: size of the fifo buffer. Must be a power of two.
 *
 * Note1: the macro can be used for global and local mbox_kfifo data type variables
 * Note2: the macro creates two objects:
 *  A mbox_kfifo object with the given name and a buffer for the mbox_kfifo
 *  object named name##mbox_kfifo_buffer
 */
#define DEFINE_MBOX_KFIFO(name, size) \
        unsigned char name##mbox_kfifo_buffer[size]; \
        struct mbox_kfifo name = __mbox_kfifo_initializer(size, name##mbox_kfifo_buffer)

extern void mbox_kfifo_init(struct mbox_kfifo *fifo, void *buffer,
                        unsigned int size);
extern __must_check int mbox_kfifo_alloc(struct mbox_kfifo *fifo, unsigned int size,
                        gfp_t gfp_mask);
extern void mbox_kfifo_free(struct mbox_kfifo *fifo);
extern unsigned int mbox_kfifo_in(struct mbox_kfifo *fifo,
                                const void *from, unsigned int len);
extern __must_check unsigned int mbox_kfifo_out(struct mbox_kfifo *fifo,
                                void *to, unsigned int len);
extern __must_check unsigned int mbox_kfifo_out_peek(struct mbox_kfifo *fifo,
                                void *to, unsigned int len, unsigned offset);

/**
 * mbox_kfifo_initialized - Check if mbox_kfifo is initialized.
 * @fifo: fifo to check
 * Return %true if FIFO is initialized, otherwise %false.
 * Assumes the fifo was 0 before.
 */
static inline bool mbox_kfifo_initialized(struct mbox_kfifo *fifo)
{
        return fifo->buffer != NULL;
}

/**
 * mbox_kfifo_reset - removes the entire FIFO contents
 * @fifo: the fifo to be emptied.
 */
static inline void mbox_kfifo_reset(struct mbox_kfifo *fifo)
{
        fifo->in = fifo->out = 0;
}

/**
 * mbox_kfifo_reset_out - skip FIFO contents
 * @fifo: the fifo to be emptied.
 */
static inline void mbox_kfifo_reset_out(struct mbox_kfifo *fifo)
{
        smp_mb();
        fifo->out = fifo->in;
}

/**
 * mbox_kfifo_size - returns the size of the fifo in bytes
 * @fifo: the fifo to be used.
 */
static inline __must_check unsigned int mbox_kfifo_size(struct mbox_kfifo *fifo)
{
        return fifo->size;
}

/**
 * mbox_kfifo_len - returns the number of used bytes in the FIFO
 * @fifo: the fifo to be used.
 */
static inline unsigned int mbox_kfifo_len(struct mbox_kfifo *fifo)
{
        register unsigned int   out;

        out = fifo->out;
        smp_rmb();
        return fifo->in - out;
}

/**
 * mbox_kfifo_is_empty - returns true if the fifo is empty
 * @fifo: the fifo to be used.
 */
static inline __must_check int mbox_kfifo_is_empty(struct mbox_kfifo *fifo)
{
        return fifo->in == fifo->out;
}

/**
 * mbox_kfifo_is_full - returns true if the fifo is full
 * @fifo: the fifo to be used.
 */
static inline __must_check int mbox_kfifo_is_full(struct mbox_kfifo *fifo)
{
        return mbox_kfifo_len(fifo) == mbox_kfifo_size(fifo);
}

/**
 * mbox_kfifo_avail - returns the number of bytes available in the FIFO
 * @fifo: the fifo to be used.
 */
static inline __must_check unsigned int mbox_kfifo_avail(struct mbox_kfifo *fifo)
{
        return mbox_kfifo_size(fifo) - mbox_kfifo_len(fifo);
}

/**
 * mbox_kfifo_in_locked - puts some data into the FIFO using a spinlock for locking
 * @fifo: the fifo to be used.
 * @from: the data to be added.
 * @n: the length of the data to be added.
 * @lock: pointer to the spinlock to use for locking.
 *
 * This function copies at most @len bytes from the @from buffer into
 * the FIFO depending on the free space, and returns the number of
 * bytes copied.
 */
static inline unsigned int mbox_kfifo_in_locked(struct mbox_kfifo *fifo,
                const void *from, unsigned int n, spinlock_t *lock)
{
        unsigned long flags;
        unsigned int ret;

        spin_lock_irqsave(lock, flags);

        ret = mbox_kfifo_in(fifo, from, n);

        spin_unlock_irqrestore(lock, flags);

        return ret;
}

/**
 * mbox_kfifo_out_locked - gets some data from the FIFO using a spinlock for locking
 * @fifo: the fifo to be used.
 * @to: where the data must be copied.
 * @n: the size of the destination buffer.
 * @lock: pointer to the spinlock to use for locking.
 *
 * This function copies at most @len bytes from the FIFO into the
 * @to buffer and returns the number of copied bytes.
 */
static inline __must_check unsigned int mbox_kfifo_out_locked(struct mbox_kfifo *fifo,
        void *to, unsigned int n, spinlock_t *lock)
{
        unsigned long flags;
        unsigned int ret;

        spin_lock_irqsave(lock, flags);

        ret = mbox_kfifo_out(fifo, to, n);

        spin_unlock_irqrestore(lock, flags);

        return ret;
}

extern void mbox_kfifo_skip(struct mbox_kfifo *fifo, unsigned int len);

extern __must_check int mbox_kfifo_from_user(struct mbox_kfifo *fifo,
        const void __user *from, unsigned int n, unsigned *lenout);

extern __must_check int mbox_kfifo_to_user(struct mbox_kfifo *fifo,
        void __user *to, unsigned int n, unsigned *lenout);

/*
 * __mbox_kfifo_add_out internal helper function for updating the out offset
 */
static inline void __mbox_kfifo_add_out(struct mbox_kfifo *fifo,
                                unsigned int off)
{
        smp_mb();
        fifo->out += off;
}

/*
 * __mbox_kfifo_add_in internal helper function for updating the in offset
 */
static inline void __mbox_kfifo_add_in(struct mbox_kfifo *fifo,
                                unsigned int off)
{
        smp_wmb();
        fifo->in += off;
}

/*
 * __mbox_kfifo_off internal helper function for calculating the index of a
 * given offeset
 */
static inline unsigned int __mbox_kfifo_off(struct mbox_kfifo *fifo, unsigned int off)
{
        return off & (fifo->size - 1);
}

/*
 * __mbox_kfifo_peek_n internal helper function for determinate the length of
 * the next record in the fifo
 */
static inline unsigned int __mbox_kfifo_peek_n(struct mbox_kfifo *fifo,
                                unsigned int recsize)
{
#define __mbox_kfifo_GET(fifo, off, shift) \
        ((fifo)->buffer[__mbox_kfifo_off((fifo), (fifo)->out+(off))] << (shift))

        unsigned int l;

        l = __mbox_kfifo_GET(fifo, 0, 0);

        if (--recsize)
                l |= __mbox_kfifo_GET(fifo, 1, 8);

        return l;
#undef  __mbox_kfifo_GET
}

/*
 * __mbox_kfifo_poke_n internal helper function for storing the length of
 * the next record into the fifo
 */
static inline void __mbox_kfifo_poke_n(struct mbox_kfifo *fifo,
                        unsigned int recsize, unsigned int n)
{
#define __mbox_kfifo_PUT(fifo, off, val, shift) \
                ( \
                (fifo)->buffer[__mbox_kfifo_off((fifo), (fifo)->in+(off))] = \
                (unsigned char)((val) >> (shift)) \
                )

        __mbox_kfifo_PUT(fifo, 0, n, 0);

        if (--recsize)
                __mbox_kfifo_PUT(fifo, 1, n, 8);
#undef  __mbox_kfifo_PUT
}

/*
 * __mbox_kfifo_in_... internal functions for put date into the fifo
 * do not call it directly, use mbox_kfifo_in_rec() instead
 */
extern unsigned int __mbox_kfifo_in_n(struct mbox_kfifo *fifo,
        const void *from, unsigned int n, unsigned int recsize);

extern unsigned int __mbox_kfifo_in_generic(struct mbox_kfifo *fifo,
        const void *from, unsigned int n, unsigned int recsize);

static inline unsigned int __mbox_kfifo_in_rec(struct mbox_kfifo *fifo,
        const void *from, unsigned int n, unsigned int recsize)
{
        unsigned int ret;

        ret = __mbox_kfifo_in_n(fifo, from, n, recsize);

        if (likely(ret == 0)) {
                if (recsize)
                        __mbox_kfifo_poke_n(fifo, recsize, n);
                __mbox_kfifo_add_in(fifo, n + recsize);
        }
        return ret;
}

/**
 * mbox_kfifo_in_rec - puts some record data into the FIFO
 * @fifo: the fifo to be used.
 * @from: the data to be added.
 * @n: the length of the data to be added.
 * @recsize: size of record field
 *
 * This function copies @n bytes from the @from into the FIFO and returns
 * the number of bytes which cannot be copied.
 * A returned value greater than the @n value means that the record doesn't
 * fit into the buffer.
 *
 * Note that with only one concurrent reader and one concurrent
 * writer, you don't need extra locking to use these functions.
 */
static inline __must_check unsigned int mbox_kfifo_in_rec(struct mbox_kfifo *fifo,
        void *from, unsigned int n, unsigned int recsize)
{
        if (!__builtin_constant_p(recsize))
                return __mbox_kfifo_in_generic(fifo, from, n, recsize);
        return __mbox_kfifo_in_rec(fifo, from, n, recsize);
}

/*
 * __mbox_kfifo_out_... internal functions for get date from the fifo
 * do not call it directly, use mbox_kfifo_out_rec() instead
 */
extern unsigned int __mbox_kfifo_out_n(struct mbox_kfifo *fifo,
        void *to, unsigned int reclen, unsigned int recsize);

extern unsigned int __mbox_kfifo_out_generic(struct mbox_kfifo *fifo,
        void *to, unsigned int n,
        unsigned int recsize, unsigned int *total);

static inline unsigned int __mbox_kfifo_out_rec(struct mbox_kfifo *fifo,
        void *to, unsigned int n, unsigned int recsize,
        unsigned int *total)
{
        unsigned int l;

        if (!recsize) {
                l = n;
                if (total)
                        *total = l;
        } else {
                l = __mbox_kfifo_peek_n(fifo, recsize);
                if (total)
                        *total = l;
                if (n < l)
                        return l;
        }

        return __mbox_kfifo_out_n(fifo, to, l, recsize);
}

/**
 * mbox_kfifo_out_rec - gets some record data from the FIFO
 * @fifo: the fifo to be used.
 * @to: where the data must be copied.
 * @n: the size of the destination buffer.
 * @recsize: size of record field
 * @total: pointer where the total number of to copied bytes should stored
 *
 * This function copies at most @n bytes from the FIFO to @to and returns the
 * number of bytes which cannot be copied.
 * A returned value greater than the @n value means that the record doesn't
 * fit into the @to buffer.
 *
 * Note that with only one concurrent reader and one concurrent
 * writer, you don't need extra locking to use these functions.
 */
static inline __must_check unsigned int mbox_kfifo_out_rec(struct mbox_kfifo *fifo,
        void *to, unsigned int n, unsigned int recsize,
        unsigned int *total)

{
        if (!__builtin_constant_p(recsize))
                return __mbox_kfifo_out_generic(fifo, to, n, recsize, total);
        return __mbox_kfifo_out_rec(fifo, to, n, recsize, total);
}

/*
 * __mbox_kfifo_from_user_... internal functions for transfer from user space into
 * the fifo. do not call it directly, use mbox_kfifo_from_user_rec() instead
 */
extern unsigned int __mbox_kfifo_from_user_n(struct mbox_kfifo *fifo,
        const void __user *from, unsigned int n, unsigned int recsize);

extern unsigned int __mbox_kfifo_from_user_generic(struct mbox_kfifo *fifo,
        const void __user *from, unsigned int n, unsigned int recsize);

static inline unsigned int __mbox_kfifo_from_user_rec(struct mbox_kfifo *fifo,
        const void __user *from, unsigned int n, unsigned int recsize)
{
        unsigned int ret;

        ret = __mbox_kfifo_from_user_n(fifo, from, n, recsize);

        if (likely(ret == 0)) {
                if (recsize)
                        __mbox_kfifo_poke_n(fifo, recsize, n);
                __mbox_kfifo_add_in(fifo, n + recsize);
        }
        return ret;
}

/**
 * mbox_kfifo_from_user_rec - puts some data from user space into the FIFO
 * @fifo: the fifo to be used.
 * @from: pointer to the data to be added.
 * @n: the length of the data to be added.
 * @recsize: size of record field
 *
 * This function copies @n bytes from the @from into the
 * FIFO and returns the number of bytes which cannot be copied.
 *
 * If the returned value is equal or less the @n value, the copy_from_user()
 * functions has failed. Otherwise the record doesn't fit into the buffer.
 *
 * Note that with only one concurrent reader and one concurrent
 * writer, you don't need extra locking to use these functions.
 */
static inline __must_check unsigned int mbox_kfifo_from_user_rec(struct mbox_kfifo *fifo,
        const void __user *from, unsigned int n, unsigned int recsize)
{
        if (!__builtin_constant_p(recsize))
                return __mbox_kfifo_from_user_generic(fifo, from, n, recsize);
        return __mbox_kfifo_from_user_rec(fifo, from, n, recsize);
}

/*
 * __mbox_kfifo_to_user_... internal functions for transfer fifo data into user space
 * do not call it directly, use mbox_kfifo_to_user_rec() instead
 */
extern unsigned int __mbox_kfifo_to_user_n(struct mbox_kfifo *fifo,
        void __user *to, unsigned int n, unsigned int reclen,
        unsigned int recsize);

extern unsigned int __mbox_kfifo_to_user_generic(struct mbox_kfifo *fifo,
        void __user *to, unsigned int n, unsigned int recsize,
        unsigned int *total);

static inline unsigned int __mbox_kfifo_to_user_rec(struct mbox_kfifo *fifo,
        void __user *to, unsigned int n,
        unsigned int recsize, unsigned int *total)
{
        unsigned int l;

        if (!recsize) {
                l = n;
                if (total)
                        *total = l;
        } else {
                l = __mbox_kfifo_peek_n(fifo, recsize);
                if (total)
                        *total = l;
                if (n < l)
                        return l;
        }

        return __mbox_kfifo_to_user_n(fifo, to, n, l, recsize);
}

/**
 * mbox_kfifo_to_user_rec - gets data from the FIFO and write it to user space
 * @fifo: the fifo to be used.
 * @to: where the data must be copied.
 * @n: the size of the destination buffer.
 * @recsize: size of record field
 * @total: pointer where the total number of to copied bytes should stored
 *
 * This function copies at most @n bytes from the FIFO to the @to.
 * In case of an error, the function returns the number of bytes which cannot
 * be copied.
 * If the returned value is equal or less the @n value, the copy_to_user()
 * functions has failed. Otherwise the record doesn't fit into the @to buffer.
 *
 * Note that with only one concurrent reader and one concurrent
 * writer, you don't need extra locking to use these functions.
 */
static inline __must_check unsigned int mbox_kfifo_to_user_rec(struct mbox_kfifo *fifo,
                void __user *to, unsigned int n, unsigned int recsize,
                unsigned int *total)
{
        if (!__builtin_constant_p(recsize))
                return __mbox_kfifo_to_user_generic(fifo, to, n, recsize, total);
        return __mbox_kfifo_to_user_rec(fifo, to, n, recsize, total);
}

/*
 * __mbox_kfifo_peek_... internal functions for peek into the next fifo record
 * do not call it directly, use mbox_kfifo_peek_rec() instead
 */
extern unsigned int __mbox_kfifo_peek_generic(struct mbox_kfifo *fifo,
                                unsigned int recsize);

/**
 * mbox_kfifo_peek_rec - gets the size of the next FIFO record data
 * @fifo: the fifo to be used.
 * @recsize: size of record field
 *
 * This function returns the size of the next FIFO record in number of bytes
 */
static inline __must_check unsigned int mbox_kfifo_peek_rec(struct mbox_kfifo *fifo,
        unsigned int recsize)
{
        if (!__builtin_constant_p(recsize))
                return __mbox_kfifo_peek_generic(fifo, recsize);
        if (!recsize)
                return mbox_kfifo_len(fifo);
        return __mbox_kfifo_peek_n(fifo, recsize);
}

/*
 * __mbox_kfifo_skip_... internal functions for skip the next fifo record
 * do not call it directly, use mbox_kfifo_skip_rec() instead
 */
extern void __mbox_kfifo_skip_generic(struct mbox_kfifo *fifo, unsigned int recsize);

static inline void __mbox_kfifo_skip_rec(struct mbox_kfifo *fifo,
        unsigned int recsize)
{
        unsigned int l;

        if (recsize) {
                l = __mbox_kfifo_peek_n(fifo, recsize);

                if (l + recsize <= mbox_kfifo_len(fifo)) {
                        __mbox_kfifo_add_out(fifo, l + recsize);
                        return;
                }
        }
        mbox_kfifo_reset_out(fifo);
}

/**
 * mbox_kfifo_skip_rec - skip the next fifo out record
 * @fifo: the fifo to be used.
 * @recsize: size of record field
 *
 * This function skips the next FIFO record
 */
static inline void mbox_kfifo_skip_rec(struct mbox_kfifo *fifo,
        unsigned int recsize)
{
        if (!__builtin_constant_p(recsize))
                __mbox_kfifo_skip_generic(fifo, recsize);
        else
                __mbox_kfifo_skip_rec(fifo, recsize);
}

/**
 * mbox_kfifo_avail_rec - returns the number of bytes available in a record FIFO
 * @fifo: the fifo to be used.
 * @recsize: size of record field
 */
static inline __must_check unsigned int mbox_kfifo_avail_rec(struct mbox_kfifo *fifo,
        unsigned int recsize)
{
        unsigned int l = mbox_kfifo_size(fifo) - mbox_kfifo_len(fifo);

        return (l > recsize) ? l - recsize : 0;
}

#endif
