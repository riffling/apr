/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 */

#include "fileio.h"
#include "apr_lock.h"

static ap_status_t wait_for_io_or_timeout(ap_file_t *file, int for_read)
{
    struct timeval tv, *tvptr;
    fd_set fdset;
    int srv;

    /* TODO - timeout should be less each time through this loop */

    do {
        FD_ZERO(&fdset);
        FD_SET(file->filedes, &fdset);
        if (file->timeout >= 0) {
            tv.tv_sec = file->timeout / AP_USEC_PER_SEC;
            tv.tv_usec = file->timeout % AP_USEC_PER_SEC;
            tvptr = &tv;
        }
        else {
            tvptr = NULL;
        }
        srv = select(FD_SETSIZE,
            for_read ? &fdset : NULL,
            for_read ? NULL : &fdset,
            NULL,
            tvptr);
    } while (srv == -1 && errno == EINTR);

    if (srv == 0) {
        return APR_TIMEUP;
    }
    else if (srv < 0) {
        return errno;
    }
    return APR_SUCCESS;
}

ap_status_t ap_read(ap_file_t *thefile, void *buf, ap_ssize_t *nbytes)
{
    ap_ssize_t rv;
    ap_ssize_t bytes_read;

    if(*nbytes <= 0) {
        *nbytes = 0;
	return APR_SUCCESS;
    }

    if (thefile->buffered) {
        char *pos = (char *)buf;
        ap_uint64_t blocksize;
        ap_uint64_t size = *nbytes;

#if APR_HAS_THREADS
        ap_lock(thefile->thlock);
#endif

        if (thefile->direction == 1) {
            ap_flush(thefile);
            thefile->bufpos = 0;
            thefile->direction = 0;
            thefile->dataRead = 0;
        }

	rv = 0;
        while (rv == 0 && size > 0) {
            if (thefile->bufpos >= thefile->dataRead) {
                thefile->dataRead = read(thefile->filedes, thefile->buffer, APR_FILE_BUFSIZE);
                if (thefile->dataRead == 0) {
                    thefile->eof_hit = TRUE;
                    break;
                }
                thefile->filePtr += thefile->dataRead;
                thefile->bufpos = 0;
            }

            blocksize = size > thefile->dataRead - thefile->bufpos ? thefile->dataRead - thefile->bufpos : size;                                                            memcpy(pos, thefile->buffer + thefile->bufpos, blocksize);
            thefile->bufpos += blocksize;
            pos += blocksize;
            size -= blocksize;
        }

        *nbytes = rv == 0 ? pos - (char *)buf : 0;
#if APR_HAS_THREADS
        ap_unlock(thefile->thlock);
#endif
        return rv;
    }
    else {
        bytes_read = 0;
        if (thefile->ungetchar != -1) {
            bytes_read = 1;
            *(char *)buf = (char)thefile->ungetchar;
            buf = (char *)buf + 1;
            (*nbytes)--;
            thefile->ungetchar = -1;
	if     (*nbytes == 0) {
	        *nbytes = bytes_read;
	        return APR_SUCCESS;
	}
        }

        do {
            rv = read(thefile->filedes, buf, *nbytes);
        } while (rv == -1 && errno == EINTR);

        if (rv == -1 && 
            (errno == EAGAIN || errno == EWOULDBLOCK) && 
            thefile->timeout != 0) {
            ap_status_t arv = wait_for_io_or_timeout(thefile, 1);
            if (arv != APR_SUCCESS) {
                *nbytes = bytes_read;
                return arv;
            }
            else {
                do {
                    rv = read(thefile->filedes, buf, *nbytes);
                } while (rv == -1 && errno == EINTR);
            }
        }  

        *nbytes = bytes_read;
        if (rv == 0) {
	return     APR_EOF;
        }
        if (rv > 0) {
	*nbytes     += rv;
	return     APR_SUCCESS;
        }
        return errno;
    }
}

ap_status_t ap_write(ap_file_t *thefile, void *buf, ap_ssize_t *nbytes)
{
    ap_size_t rv;

    if (thefile->buffered) {
        char *pos = (char *)buf;
        int blocksize;
        int size = *nbytes;

#if APR_HAS_THREADS
        ap_lock(thefile->thlock);
#endif

        if ( thefile->direction == 0 ) {
            /* Position file pointer for writing at the offset we are 
             * logically reading from
             */
            ap_int64_t offset = thefile->filePtr - thefile->dataRead + thefile->bufpos;
            if (offset != thefile->filePtr)
                lseek(thefile->filedes, offset, SEEK_SET);
            thefile->bufpos = thefile->dataRead = 0;
            thefile->direction = 1;
        }

	rv = 0;
        while (rv == 0 && size > 0) {
            if (thefile->bufpos == APR_FILE_BUFSIZE)   /* write buffer is full*/
                ap_flush(thefile);

            blocksize = size > APR_FILE_BUFSIZE - thefile->bufpos ? 
                        APR_FILE_BUFSIZE - thefile->bufpos : size;
            memcpy(thefile->buffer + thefile->bufpos, pos, blocksize);                      thefile->bufpos += blocksize;
            pos += blocksize;
            size -= blocksize;
        }

#if APR_HAS_THREADS
        ap_unlock(thefile->thlock);
#endif
        return rv;
    }
    else {
        do {
            rv = write(thefile->filedes, buf, *nbytes);
        } while (rv == (ap_size_t)-1 && errno == EINTR);

        if (rv == (ap_size_t)-1 &&
            (errno == EAGAIN || errno == EWOULDBLOCK) && 
            thefile->timeout != 0) {
            ap_status_t arv = wait_for_io_or_timeout(thefile, 0);
            if (arv != APR_SUCCESS) {
                *nbytes = 0;
                return arv;
            }
            else {
                do {
                    rv = write(thefile->filedes, buf, *nbytes);
	        } while (rv == (ap_size_t)-1 && errno == EINTR);
            }
        }  

        if (rv == (ap_size_t)-1) {
            (*nbytes) = 0;
            return errno;
        }
        *nbytes = rv;
        return APR_SUCCESS;
    }
}

ap_status_t ap_writev(ap_file_t *thefile, const struct iovec *vec,
                      ap_size_t nvec, ap_ssize_t *nbytes)
{
#ifdef HAVE_WRITEV
    int bytes;

    if ((bytes = writev(thefile->filedes, vec, nvec)) < 0) {
        *nbytes = 0;
        return errno;
    }
    else {
        *nbytes = bytes;
        return APR_SUCCESS;
    }
#else
    *nbytes = vec[0].iov_len;
    return ap_write(thefile, vec[0].iov_base, nbytes);
#endif
}

ap_status_t ap_putc(char ch, ap_file_t *thefile)
{
    if (write(thefile->filedes, &ch, 1) != 1) {
        return errno;
    }
    return APR_SUCCESS; 
}

ap_status_t ap_ungetc(char ch, ap_file_t *thefile)
{
    thefile->ungetchar = (unsigned char)ch;
    return APR_SUCCESS; 
}

ap_status_t ap_getc(char *ch, ap_file_t *thefile)
{
    ssize_t rv;
    
    if (thefile->ungetchar != -1) {
        *ch = (char) thefile->ungetchar;
        thefile->ungetchar = -1;
        return APR_SUCCESS;
    }
    rv = read(thefile->filedes, ch, 1); 
    if (rv == 0) {
        thefile->eof_hit = TRUE;
        return APR_EOF;
    }
    else if (rv != 1) {
        return errno;
    }
    return APR_SUCCESS; 
}

ap_status_t ap_puts(char *str, ap_file_t *thefile)
{
    ssize_t rv;
    int len;

    len = strlen(str);
    rv = write(thefile->filedes, str, len); 
    if (rv != len) {
        return errno;
    }
    return APR_SUCCESS; 
}

ap_status_t ap_flush(ap_file_t *thefile)
{
    if (thefile->buffered) {
        ap_int64_t written = 0;
        int rc = 0;

        if (thefile->direction == 1 && thefile->bufpos) {
            written= write(thefile->filedes, thefile->buffer, thefile->bufpos);
            thefile->filePtr += written;

            if (rc == 0)
                thefile->bufpos = 0;
        }

        return rc;
    }
    /* There isn't anything to do if we aren't buffering the output
     * so just return success.
     */
    return APR_SUCCESS; 
}

ap_status_t ap_fgets(char *str, int len, ap_file_t *thefile)
{
    ssize_t rv;
    int i, used_unget = FALSE, beg_idx;

    if(len <= 1)  /* as per fgets() */
        return APR_SUCCESS;

    if(thefile->ungetchar != -1){
        str[0] = thefile->ungetchar;
	used_unget = TRUE;
	beg_idx = 1;
	if(str[0] == '\n' || str[0] == '\r'){
	    thefile->ungetchar = -1;
	    str[1] = '\0';
	    return APR_SUCCESS;
	}
    } else
        beg_idx = 0;
    
    for (i = beg_idx; i < len; i++) {
        rv = read(thefile->filedes, &str[i], 1); 
        if (rv == 0) {
            thefile->eof_hit = TRUE;
	    if(used_unget) thefile->filedes = -1;
	    str[i] = '\0';
            return APR_EOF;
        }
        else if (rv != 1) {
            return errno;
        }
        if (str[i] == '\n' || str[i] == '\r')
            break;
    }
    if (i < len-1)
        str[i+1] = '\0';
    return APR_SUCCESS; 
}

#if 0 /* not currently used */
static int printf_flush(ap_vformatter_buff_t *vbuff)
{
    /* I would love to print this stuff out to the file, but I will
     * get that working later.  :)  For now, just return.
     */
    return -1;
}
#endif

APR_EXPORT(int) ap_fprintf(ap_file_t *fptr, const char *format, ...)
{
    int cc;
    va_list ap;
    char *buf;
    int len;

    buf = malloc(HUGE_STRING_LEN);
    if (buf == NULL) {
        return 0;
    }
    va_start(ap, format);
    len = ap_vsnprintf(buf, HUGE_STRING_LEN, format, ap);
    cc = ap_puts(buf, fptr);
    va_end(ap);
    free(buf);
    return (cc == APR_SUCCESS) ? len : -1;
}

ap_status_t ap_file_check_read(ap_file_t *fd)
{
    fd_set fds;
    int rv;
    struct timeval tv = { 0 };

    FD_ZERO(&fds);
    FD_SET(fd->filedes, &fds);
    if ((rv = select(fd->filedes + 1, &fds, NULL, NULL, &tv)) == -1) {
        return errno;
    }
    else if (rv == 0) {
        return APR_TIMEUP;
    }
    else {
        return APR_SUCCESS;
    }
}

