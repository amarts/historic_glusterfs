/*
  (C) 2007 Z RESEARCH Inc. <http://www.zresearch.com>
  
  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.
    
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.
    
  You should have received a copy of the GNU General Public
  License along with this program; if not, write to the Free
  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301 USA
*/ 

/* TODO: striping requires a review and cleanup */
#include "xlator.h"
#include "logging.h"
#include <fnmatch.h>


#define LOCK_INIT(x)    pthread_mutex_init (x, NULL);
#define LOCK(x)         pthread_mutex_lock (x);
#define UNLOCK(x)       pthread_mutex_unlock (x);
#define LOCK_DESTROY(x) pthread_mutex_destroy (x);

struct stripe_local;

struct stripe_options {
  struct stripe_options *next;
  char path_pattern[256];
  int32_t block_size;
};
struct stripe_private {
  struct stripe_options *pattern;
  int32_t child_count;
};

struct readv_replies {
  struct iovec *vector;
  int32_t count; //count of vector
  int32_t op_ret;   //op_ret of readv
  int32_t op_errno;
};

struct stripe_local {
  struct stripe_local *next;
  call_frame_t *orig_frame; //

  /* Used by _cbk functions */
  int32_t call_count;
  int32_t wind_count; // used instead of child_cound in case of read and write */
  int32_t op_ret;
  int32_t op_errno; 
  int32_t count;
  int32_t unwind;
  struct stat stbuf;
  struct flock lock;
  struct readv_replies *replies;
  struct statvfs statvfs_buf;
  dir_entry_t *entry;
  struct xlator_stats stats;
  /* For File I/O fops */
  dict_t *ctx;

  /* General usage */
  off_t offset;
  off_t stripe_size;
  int32_t node_index;

  int32_t failed;
};

typedef struct stripe_local   stripe_local_t;
typedef struct stripe_private stripe_private_t;

static int32_t 
stripe_get_matching_bs (const char *path, struct stripe_options *opts) 
{
  struct stripe_options *trav = opts;
  char *filename = strdup (path);
  char *file = basename (filename);
  while (trav) {
    if (fnmatch (trav->path_pattern, file, FNM_PATHNAME) == 0) {
      free (filename);
      return trav->block_size;
    }
    trav = trav->next;
  }
  free (filename);
  return 0;
}

static int32_t
stripe_setxattr_cbk (call_frame_t *frame,
		     call_frame_t *prev_frame,
		     xlator_t *xl,
		     int32_t op_ret,
		     int32_t op_errno)
{
  stripe_local_t *local = (stripe_local_t *) frame->local;
  int32_t callcnt;

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
  }
  return 0;
}

static int32_t
stripe_setxattr (call_frame_t *frame,
		 xlator_t *xl,
		 const char *path,
		 const char *name,
		 const char *value,
		 size_t size,
		 int32_t flags)
{
  stripe_local_t *local = (void *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND(frame,
	       stripe_setxattr_cbk,
	       trav->xlator,
	       trav->xlator->fops->setxattr,
	       path,
	       name,
	       value,
	       size,
	       flags);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_getxattr_cbk (call_frame_t *frame,
		     call_frame_t *prev_frame,
		     xlator_t *xl,
		     int32_t op_ret,
		     int32_t op_errno,
		     void *value)
{
  STACK_UNWIND (frame, op_ret, op_errno, value);
  return 0;
}

static int32_t
stripe_getxattr (call_frame_t *frame,
		 xlator_t *xl,
		 const char *path,
		 const char *name,
		 size_t size)
{
  xlator_t *stripexl = xl->children->xlator;

  STACK_WIND (frame,
	      stripe_getxattr_cbk,
	      stripexl,
	      stripexl->fops->getxattr,
	      path,
	      name,
	      size);
  return 0;
}

static int32_t
stripe_listxattr_cbk (call_frame_t *frame,
		      call_frame_t *prev_frame,
		      xlator_t *xl,
		      int32_t op_ret,
		      int32_t op_errno,
		      void *value)
{
  STACK_UNWIND (frame, op_ret, op_errno, value);
  return 0;
}

static int32_t
stripe_listxattr (call_frame_t *frame,
		  xlator_t *xl,
		  const char *path,
		  size_t size)
{ 
  xlator_t *stripexl = xl->children->xlator;
  STACK_WIND (frame,
	      stripe_listxattr_cbk,
	      stripexl,
	      stripexl->fops->listxattr,
	      path,
	      size);
  return 0;
}

static int32_t
stripe_removexattr_cbk (call_frame_t *frame,
		     call_frame_t *prev_frame,
		     xlator_t *xl,
		     int32_t op_ret,
		     int32_t op_errno)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }
  
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
  }
  return 0;
}

static int32_t
stripe_removexattr (call_frame_t *frame,
		    xlator_t *xl,
		    const char *path,
		    const char *name)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;

  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND (frame,
		stripe_removexattr_cbk,
		trav->xlator,
		trav->xlator->fops->removexattr,
		path,
		name);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_open_failed_release_cbk (call_frame_t *frame,
				call_frame_t *prev_frame,
				xlator_t *xl,
				int32_t op_ret,
				int32_t op_errno)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);
    
  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_DESTROY (frame->root);
  }
  return 0;
}

static int32_t
stripe_open_cbk (call_frame_t *frame,
		 call_frame_t *prev_frame,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno,
		 dict_t *file_ctx,
		 struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  dict_t *ctx = local->ctx;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1 ) {
    local->op_ret = -1;
    local->op_errno = op_errno;
    local->failed = 1;
  } 
  if (op_ret >= 0) {
    LOCK(&frame->mutex);
    dict_set (ctx, prev_frame->this->name, int_to_data((long)file_ctx));
    local->op_ret = op_ret;
    UNLOCK (&frame->mutex);
  }

  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    if (!local->failed) {
      dict_set (ctx, frame->this->name, int_to_data (local->stripe_size));
      LOCK_DESTROY(&frame->mutex);
      STACK_UNWIND (frame, local->op_ret, local->op_errno, ctx, stbuf);
    } else {
      xlator_list_t *trav = xl->children;
      stripe_local_t *rlocal = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
      call_frame_t *release_frame = copy_frame (frame);
      release_frame->local = rlocal;
      LOCK_INIT (&release_frame->mutex);
      while (trav) {
	data_t *ctx_data = dict_get (ctx, trav->xlator->name);
	if (!ctx_data) {
	  LOCK (&release_frame->mutex);
	  rlocal->call_count++;
	  UNLOCK (&release_frame->mutex);
	  trav = trav->next;
	  continue;
	}
	dict_t *ctx = (void *)((long)data_to_int(ctx_data));
	STACK_WIND (release_frame,
		    stripe_open_failed_release_cbk,
		    trav->xlator,
		    trav->xlator->fops->release,
		    ctx);
	trav = trav->next;
      }
      LOCK_DESTROY (&frame->mutex);
      STACK_UNWIND (frame, -1, local->op_errno, NULL, NULL);
    }
  }
  return 0;
}

static int32_t
stripe_open (call_frame_t *frame,
	     xlator_t *xl,
	     const char *path,
	     int32_t flags,
	     mode_t mode)
{
  stripe_private_t *priv = xl->private;
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;

  local->ctx = get_new_dict ();
  local->stripe_size = stripe_get_matching_bs (path, priv->pattern);
  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND (frame,
		stripe_open_cbk,
		trav->xlator,
		trav->xlator->fops->open,
		path,
		flags,
		mode);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_readv_cbk (call_frame_t *frame,
		  call_frame_t *prev_frame,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct iovec *vector,
		  int32_t count)
{
  stripe_local_t *local = frame->local;
  call_frame_t *main_frame = local->orig_frame;
  stripe_local_t *main_local = main_frame->local;
  int32_t callcnt;
  int32_t index = local->node_index;


  LOCK (&main_frame->mutex);

  main_local->replies[index].op_ret = op_ret;
  main_local->replies[index].op_errno = op_errno;
  if (op_ret > 0) {
    main_local->replies[index].count  = count;
    main_local->replies[index].vector = iov_dup (vector, count);
    dict_copy (frame->root->rsp_refs, main_frame->root->rsp_refs);
  }
  callcnt = ++main_local->call_count;

  UNLOCK(&main_frame->mutex);
  
  if (callcnt == main_local->wind_count) {
    dict_t *refs = main_frame->root->rsp_refs;
    int32_t index = 0;
    int32_t final_count = 0;
    struct iovec *final_vec = NULL;

    op_ret = 0;
    for (index=0; index < main_local->wind_count; index++) {
      /* TODO: check whether each stripe returned 'expected'
	 number of bytes */
      if (main_local->replies[index].op_ret == -1) {
	op_ret = -1;
	op_errno = main_local->replies[index].op_errno;
	break;
      }
      op_ret += main_local->replies[index].op_ret;
      final_count += main_local->replies[index].count;
    }

    if (op_ret != -1) {
      final_vec = calloc (final_count, sizeof (struct iovec));
      final_count = 0;

      for (index=0; index < main_local->wind_count; index++) {
	memcpy (final_vec + final_count,
		main_local->replies[index].vector,
		main_local->replies[index].count * sizeof (struct iovec));
	final_count += main_local->replies[index].count;

	free (main_local->replies[index].vector);
      }
    } else {
      final_vec = NULL;
      final_count = 0;
    }

    refs = main_frame->root->rsp_refs;

    LOCK_DESTROY (&main_frame->mutex);
    STACK_UNWIND (main_frame, op_ret, op_errno, final_vec, final_count);

    dict_unref (refs);
    free (final_vec);
  }

  STACK_DESTROY (frame->root);
  return 0;
}

static int32_t
stripe_readv (call_frame_t *frame,
	      xlator_t *xl,
	      dict_t *file_ctx,
	      size_t size,
	      off_t offset)
{
  stripe_local_t *local = calloc (1, sizeof (stripe_local_t));
  stripe_private_t *priv = xl->private;

  off_t stripe_size = data_to_int (dict_get (file_ctx,
					     frame->this->name));
  if (!stripe_size)
    stripe_size = (offset+size);

  off_t rounded_start = floor (offset, stripe_size);
  off_t rounded_end = roof (offset+size, stripe_size);
  int32_t num_stripe = (rounded_end - rounded_start) / stripe_size;
  xlator_list_t *trav = xl->children;
  int32_t index = 0;
  off_t frame_offset = offset;
  size_t frame_size;

  LOCK_INIT (&frame->mutex);
  local->wind_count = num_stripe;
  frame->local = local;
  frame->root->rsp_refs = dict_ref (get_new_dict ());

  /* This is where all the vectors should be copied. */
  local->replies = calloc (1, num_stripe * sizeof (struct readv_replies));

  for (index = 0;
       index < ((offset / stripe_size) % priv->child_count);
       index++) {
    trav = trav->next;
  }

  for (index = 0; index < num_stripe; index++) {
    call_frame_t *rframe = copy_frame (frame);
    stripe_local_t *rlocal = calloc (1, sizeof (stripe_local_t));
    data_t *ctx_data = dict_get (file_ctx, trav->xlator->name);
    dict_t *ctx = (void *)((long)data_to_int(ctx_data));

    frame_size = min (roof (frame_offset+1, stripe_size),
		      (offset + size)) - frame_offset;

    rlocal->node_index = index;
    rlocal->orig_frame = frame;
    rlocal->offset = frame_offset;
    rframe->local = rlocal;

    STACK_WIND (rframe, 
		stripe_readv_cbk,
		trav->xlator,
		trav->xlator->fops->readv,
		ctx,
		frame_size,
		frame_offset);

    frame_offset += frame_size;
    trav = (trav->next ? trav->next : xl->children);
  }

  return 0;
}

static int32_t
stripe_writev_cbk (call_frame_t *frame,
		   call_frame_t *prev_frame,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_errno = op_errno;
    local->op_ret = -1;
  }
  if (op_ret >= 0) {
    LOCK (&frame->mutex);
    local->op_ret += op_ret;
    UNLOCK (&frame->mutex);
  }

  if ((callcnt == local->wind_count) && local->unwind) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
  }
  return 0;
}

static int32_t
stripe_writev (call_frame_t *frame,
	       xlator_t *xl,
	       dict_t *file_ctx,
	       struct iovec *vector,
	       int32_t count,
	       off_t offset)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  int32_t total_size = 0;
  int32_t i = 0;
  int32_t tmp_count = count;
  struct iovec *tmp_vec = vector;
  stripe_private_t *priv = xl->private;

  local->stripe_size = data_to_int (dict_get (file_ctx, frame->this->name));

  for (i = 0; i< count; i++) {
    total_size += tmp_vec[i].iov_len;
  }
  frame->local = local;

  LOCK_INIT (&frame->mutex);

  int32_t offset_offset = 0;
  int32_t remaining_size = total_size;
  int32_t fill_size = 0;
  while (1) {
    xlator_list_t *trav = xl->children;

    if (local->stripe_size) {
      int32_t idx = ((offset + offset_offset) / local->stripe_size) % priv->child_count;
      while (idx) {
	trav = trav->next;
	idx--;
      }
      fill_size = local->stripe_size - ((offset + offset_offset) % local->stripe_size);
      if (fill_size > remaining_size)
	fill_size = remaining_size;
      remaining_size -= fill_size;
    } else {
      fill_size = total_size;
      remaining_size = 0;
    }
    tmp_count = iov_subset (vector, count, offset_offset, 
			    offset_offset + fill_size, NULL);
    tmp_vec = calloc (tmp_count, sizeof (struct iovec));
    tmp_count = iov_subset (vector, count, offset_offset, 
			      offset_offset + fill_size, tmp_vec);
    
    data_t *ctx_data = dict_get (file_ctx, trav->xlator->name);
    dict_t *ctx = (void *)((long)data_to_int(ctx_data));
    local->wind_count++;
    if (remaining_size == 0)
      local->unwind = 1;
    STACK_WIND(frame,
	       stripe_writev_cbk,
	       trav->xlator,
	       trav->xlator->fops->writev,
	       ctx,
	       tmp_vec,
	       tmp_count,
	       offset + offset_offset);
    free (tmp_vec);
    offset_offset += fill_size;
    if (remaining_size == 0)
      break;
  }
  return 0;
}

static int32_t
stripe_ftruncate_cbk (call_frame_t *frame,
		      call_frame_t *prev_frame,
		      xlator_t *xl,
		      int32_t op_ret,
		      int32_t op_errno,
		      struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, stbuf);
  }
  return 0;
}

static int32_t
stripe_ftruncate (call_frame_t *frame,
		  xlator_t *xl,
		  dict_t *file_ctx,
		  off_t offset)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  LOCK_INIT (&frame->mutex);

  while (trav) {
    data_t *ctx_data = dict_get (file_ctx, trav->xlator->name);
    dict_t *ctx = (void *)((long)data_to_int(ctx_data));
    STACK_WIND (frame,
		stripe_ftruncate_cbk,
		trav->xlator,
		trav->xlator->fops->ftruncate,
		ctx,
		offset);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_fgetattr_cbk (call_frame_t *frame,
		     call_frame_t *prev_frame,
		     xlator_t *xl,
		     int32_t op_ret,
		     int32_t op_errno,
		     struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }
  if (op_ret == 0) {
    local->stbuf = *stbuf;
    LOCK (&frame->mutex);
    if (local->stbuf.st_size < stbuf->st_size)
      local->stbuf.st_size = stbuf->st_size;
    local->stbuf.st_blocks += stbuf->st_blocks;
    if (local->stbuf.st_blksize != stbuf->st_blksize) {
      //TODO: add to blocks in terms of original block size 
    }
    UNLOCK (&frame->mutex);
  }
  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
  }
  return 0;
}

static int32_t
stripe_fgetattr (call_frame_t *frame,
		 xlator_t *xl,
		 dict_t *file_ctx)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;

  LOCK_INIT (&frame->mutex);

  while (trav) {
    data_t *ctx_data = dict_get (file_ctx, trav->xlator->name);
    dict_t *ctx = (void *)((long)data_to_int(ctx_data));
    STACK_WIND (frame,
		stripe_fgetattr_cbk,
		trav->xlator,
		trav->xlator->fops->fgetattr,
		ctx);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_flush_cbk (call_frame_t *frame,
	       call_frame_t *prev_frame,
	       xlator_t *xl,
	       int32_t op_ret,
	       int32_t op_errno)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
  }
  return 0;
}

static int32_t
stripe_flush (call_frame_t *frame,
	   xlator_t *xl,
	   dict_t *file_ctx)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  LOCK_INIT (&frame->mutex);
  while(trav) {
    data_t *ctx_data = dict_get (file_ctx, trav->xlator->name);
    if (!ctx_data) {
      gf_log ("", 1, "yaake");
      trav = trav->next;
      continue;
    }
    dict_t *ctx = (void *)((long)data_to_int(ctx_data));
    STACK_WIND (frame,
		stripe_flush_cbk,
		trav->xlator,
		trav->xlator->fops->flush,
		ctx);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_release_cbk (call_frame_t *frame,
		    call_frame_t *prev_frame,
		    xlator_t *xl,
		    int32_t op_ret,
		    int32_t op_errno)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }
    
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
  }
  return 0;
}

static int32_t
stripe_release (call_frame_t *frame,
		xlator_t *xl,
		dict_t *file_ctx)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;

  LOCK_INIT (&frame->mutex);

  while (trav) {
    data_t *ctx_data = dict_get (file_ctx, trav->xlator->name);
    dict_t *ctx = (void *)((long)data_to_int(ctx_data));
    STACK_WIND (frame,
		stripe_release_cbk,
		trav->xlator,
		trav->xlator->fops->release,
		ctx);
    trav = trav->next;
  }
  dict_destroy (file_ctx);
  return 0;
}

static int32_t
stripe_fsync_cbk (call_frame_t *frame,
		  call_frame_t *prev_frame,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
  }
  return 0;
}

static int32_t
stripe_fsync (call_frame_t *frame,
	      xlator_t *xl,
	      dict_t *file_ctx,
	      int32_t flags)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  LOCK_INIT (&frame->mutex);

  while (trav) {
    data_t *ctx_data = dict_get (file_ctx, trav->xlator->name);
    dict_t *ctx = (void *)((long)data_to_int(ctx_data));
    STACK_WIND (frame,
		stripe_fsync_cbk,
		trav->xlator,
		trav->xlator->fops->fsync,
		ctx,
		flags);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_lk_cbk (call_frame_t *frame,
	       call_frame_t *prev_frame,
	       xlator_t *xl,
	       int32_t op_ret,
	       int32_t op_errno,
	       struct flock *lock)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }
  if (op_ret == 0) {
    local->lock = *lock;
  }
  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->lock);
  }
  return 0;
}

static int32_t
stripe_lk (call_frame_t *frame,
	   xlator_t *xl,
	   dict_t *file_ctx,
	   int32_t cmd,
	   struct flock *lock)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  LOCK_INIT (&frame->mutex);

  while (trav) { 
    data_t *ctx_data = dict_get (file_ctx, trav->xlator->name);
    dict_t *ctx = (void *)((long)data_to_int(ctx_data));
    
    STACK_WIND (frame,
		stripe_lk_cbk,
		trav->xlator,
		trav->xlator->fops->lk,
		ctx,
		cmd,
		lock);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_getattr_cbk (call_frame_t *frame,
		    call_frame_t *prev_frame,
		    xlator_t *xl,
		    int32_t op_ret,
		    int32_t op_errno,
		    struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if ((op_ret == -1)) {
    local->op_ret = op_ret;
    local->op_errno = op_errno;
  } 
  if (op_ret == 0) {
    if (local->op_ret == -1)
      local->stbuf = *stbuf;
    local->op_ret = 0;
    LOCK (&frame->mutex);
    if (local->stbuf.st_size < stbuf->st_size)
      local->stbuf.st_size = stbuf->st_size;
    local->stbuf.st_blocks += stbuf->st_blocks;
    if (local->stbuf.st_blksize != stbuf->st_blksize) {
      //TODO: add to blocks in terms of original block size 
    }
    UNLOCK (&frame->mutex);
  }

  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
  }
  
  return 0;
}

static int32_t
stripe_getattr (call_frame_t *frame,
		xlator_t *xl,
		const char *path)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  local->op_ret = -1;
  LOCK_INIT (&frame->mutex);

  while (trav) {
    STACK_WIND (frame,
		stripe_getattr_cbk,
		trav->xlator,
		trav->xlator->fops->getattr,
		path);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_statfs_cbk (call_frame_t *frame,
		   call_frame_t *prev_frame,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct statvfs *stbuf)
{
  stripe_local_t *local = (stripe_local_t *)frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret != 0 && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }
  if (op_ret == 0) {
    LOCK(&frame->mutex);
    struct statvfs *dict_buf = &local->statvfs_buf;
    dict_buf->f_bsize   = stbuf->f_bsize;
    dict_buf->f_frsize  = stbuf->f_frsize;
    dict_buf->f_blocks += stbuf->f_blocks;
    dict_buf->f_bfree  += stbuf->f_bfree;
    dict_buf->f_bavail += stbuf->f_bavail;
    dict_buf->f_files  += stbuf->f_files;
    dict_buf->f_ffree  += stbuf->f_ffree;
    dict_buf->f_favail += stbuf->f_favail;
    dict_buf->f_fsid    = stbuf->f_fsid;
    dict_buf->f_flag    = stbuf->f_flag;
    dict_buf->f_namemax = stbuf->f_namemax;
    local->op_ret = 0;
    UNLOCK (&frame->mutex);
  }
  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->statvfs_buf);
  }
  return 0;
}


static int32_t
stripe_statfs (call_frame_t *frame,
	       xlator_t *xl,
	       const char *path)
{
  frame->local = (void *)calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;

  ((stripe_local_t *)frame->local)->op_ret = -1;
  ((stripe_local_t *)frame->local)->op_errno = ENOTCONN;
  LOCK_INIT (&frame->mutex);

  while (trav) {
    STACK_WIND (frame, 
		stripe_statfs_cbk,
		trav->xlator,
		trav->xlator->fops->statfs,
		path);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_truncate_cbk (call_frame_t *frame,
		     call_frame_t *prev_frame,
		     xlator_t *xl,
		     int32_t op_ret,
		     int32_t op_errno,
		     struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, stbuf);
  }
  return 0;
}

static int32_t
stripe_truncate (call_frame_t *frame,
		 xlator_t *xl,
		 const char *path,
		 off_t offset)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;

  LOCK_INIT (&frame->mutex);

  while (trav) {
    STACK_WIND (frame,
		stripe_truncate_cbk,
		trav->xlator,
		trav->xlator->fops->truncate,
		path,
		offset);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_utimes_cbk (call_frame_t *frame,
		   call_frame_t *prev_frame,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, stbuf);
  }
  return 0;
}

static int32_t
stripe_utimes (call_frame_t *frame,
	       xlator_t *xl,
	       const char *path,
	       struct timespec *buf)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND (frame,
		stripe_utimes_cbk,
		trav->xlator,
		trav->xlator->fops->utimes,
		path,
		buf);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_readlink_cbk (call_frame_t *frame,
		     call_frame_t *prev_frame,
		     xlator_t *xl,
		     int32_t op_ret,
		     int32_t op_errno,
		     char *buf)
{
  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t
stripe_readlink (call_frame_t *frame,
		 xlator_t *xl,
		 const char *path,
		 size_t size)
{
  xlator_t *stripexl = xl->children->xlator;
  
  STACK_WIND (frame,
	      stripe_readlink_cbk,
	      stripexl,
	      stripexl->fops->readlink,
	      path,
	      size);
  return 0;
}

static int32_t
stripe_readdir_cbk (call_frame_t *frame,
		    call_frame_t *prev_frame,
		    xlator_t *xl,
		    int32_t op_ret,
		    int32_t op_errno,
		    dir_entry_t *entry,
		    int32_t count)
{
  /* TODO: Currently the assumption is the direntry_t structure will be 
     having file info in same order in all the nodes. If its not, then 
     this wont work. */
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1 && op_errno != ENOTCONN && op_errno != ENOENT) {
    local->op_errno = op_errno;
  } 
  if (op_ret == 0) {
    LOCK (&frame->mutex);
    local->op_ret = op_ret;
    if (!local->entry) {
      dir_entry_t *trav = entry->next;
      local->entry = calloc (1, sizeof (dir_entry_t));
      entry->next = NULL;
      local->entry->next = trav;
      local->count = count;
    } else {
      /* update stat of all the entries */
      dir_entry_t *trav_local = local->entry->next;
      dir_entry_t *trav = entry->next;
      while (trav && trav_local) {
	trav_local->buf.st_size += trav->buf.st_size;
	trav_local->buf.st_blocks += trav->buf.st_blocks;
	trav_local->buf.st_blksize += trav->buf.st_blksize;

	trav = trav->next;
	trav_local = trav_local->next;
      }
    }
    UNLOCK (&frame->mutex);
  }
  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    dir_entry_t *prev = local->entry;
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, local->entry, local->count);
    dir_entry_t *trav = prev->next;
    while (trav) {
      prev->next = trav->next;
      free (trav->name);
      free (trav);
      trav = prev->next;
    }
    free (prev);
  }

  return 0;
}

static int32_t
stripe_readdir (call_frame_t *frame,
		xlator_t *xl,
		const char *path)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  local->op_errno = ENOENT;
  local->op_ret = -1;
  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND (frame,
		stripe_readdir_cbk,
		trav->xlator,
		trav->xlator->fops->readdir,
		path);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_mkdir_cbk (call_frame_t *frame,
		  call_frame_t *prev_frame,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    STACK_UNWIND (frame, local->op_ret, local->op_errno, stbuf);
    LOCK_DESTROY(&frame->mutex);
  }
  return 0;
}

static int32_t
stripe_mkdir (call_frame_t *frame,
	      xlator_t *xl,
	      const char *path,
	      mode_t mode)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  LOCK_INIT (&frame->mutex);

  while (trav) {
    STACK_WIND (frame,
		stripe_mkdir_cbk,
		trav->xlator,
		trav->xlator->fops->mkdir,
		path,
		mode);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_unlink_cbk (call_frame_t *frame,
		   call_frame_t *prev_frame,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
  }
  return 0;
}

static int32_t
stripe_unlink (call_frame_t *frame,
	       xlator_t *xl,
	       const char *path)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;

  LOCK_INIT (&frame->mutex);

  while (trav) {
    STACK_WIND (frame,
		stripe_unlink_cbk,
		trav->xlator,
		trav->xlator->fops->unlink,
		path);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_rmdir_cbk (call_frame_t *frame,
		  call_frame_t *prev_frame,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  if (callcnt == ((stripe_private_t*) xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
  }
  return 0;
}

static int32_t
stripe_rmdir (call_frame_t *frame,
	      xlator_t *xl,
	      const char *path)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;

  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND (frame,
		stripe_rmdir_cbk,
		trav->xlator,
		trav->xlator->fops->rmdir,
		path);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_create_cbk (call_frame_t *frame,
		   call_frame_t *prev_frame,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno,
		   dict_t *file_ctx,
		   struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  dict_t *ctx = local->ctx;
  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
    local->failed = 1;
  } 
  if (op_ret == 0) {
    LOCK (&frame->mutex);
    dict_set (ctx, prev_frame->this->name, int_to_data((long)file_ctx));
    UNLOCK (&frame->mutex);
    local->op_ret = op_ret;
  }
  
  if (callcnt == ((stripe_private_t *)xl->private)->child_count) {
    if (!local->failed) {
      dict_set (ctx, frame->this->name, int_to_data (local->stripe_size));
      LOCK_DESTROY(&frame->mutex);
      STACK_UNWIND (frame, local->op_ret, local->op_errno, ctx, stbuf);
    } else {
      xlator_list_t *trav = xl->children;
      stripe_local_t *rlocal = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
      call_frame_t *release_frame = copy_frame (frame);
      release_frame->local = rlocal;
      LOCK_INIT (&release_frame->mutex);
      while (trav) {
	data_t *ctx_data = dict_get (ctx, trav->xlator->name);
	if (!ctx_data) {
	  LOCK (&release_frame->mutex);
	  rlocal->call_count++;
	  UNLOCK (&release_frame->mutex);
	  trav = trav->next;
	  continue;
	}
	dict_t *ctx = (void *)((long)data_to_int(ctx_data));
	STACK_WIND (release_frame,
		    stripe_open_failed_release_cbk,
		    trav->xlator,
		    trav->xlator->fops->release,
		    ctx);
	trav = trav->next;
      }
      LOCK_DESTROY (&frame->mutex);
      STACK_UNWIND (frame, -1, local->op_errno, NULL, NULL);
    }

  }
  return 0;
}

static int32_t
stripe_create (call_frame_t *frame,
	       xlator_t *xl,
	       const char *path,
	       mode_t mode)
{
  stripe_private_t *priv = xl->private;
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  local->ctx = get_new_dict (); 
  local->stripe_size = stripe_get_matching_bs (path, priv->pattern);
  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND (frame,
		stripe_create_cbk,
		trav->xlator,
		trav->xlator->fops->create,
		path,
		mode);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_symlink_cbk (call_frame_t *frame,
		    call_frame_t *prev_frame,
		    xlator_t *xl,
		    int32_t op_ret,
		    int32_t op_errno,
		    struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, stbuf);
  }
  return 0;
}

static int32_t
stripe_symlink (call_frame_t *frame,
		xlator_t *xl,
		const char *oldpath,
		const char *newpath)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;

  LOCK_INIT (&frame->mutex);

  while (trav) {
    STACK_WIND (frame,
		stripe_symlink_cbk,
		trav->xlator,
		trav->xlator->fops->symlink,
		oldpath,
		newpath);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_rename_cbk (call_frame_t *frame,
		   call_frame_t *prev_frame,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }
   
  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
  }
  return 0;
}

static int32_t
stripe_rename (call_frame_t *frame,
	       xlator_t *xl,
	       const char *oldpath,
	       const char *newpath)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND (frame,
		stripe_rename_cbk,
		trav->xlator,
		trav->xlator->fops->rename,
		oldpath,
		newpath);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_link_cbk (call_frame_t *frame,
		 call_frame_t *prev_frame,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }
   
  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, stbuf);
  }
  return 0;
}

static int32_t
stripe_link (call_frame_t *frame,
	     xlator_t *xl,
	     const char *oldpath,
	     const char *newpath)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;

  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND (frame,
		stripe_link_cbk,
		trav->xlator,
		trav->xlator->fops->link,
		oldpath,
		newpath);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_chmod_cbk (call_frame_t *frame,
		  call_frame_t *prev_frame,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, stbuf);
  }
  return 0;
}

static int32_t
stripe_chmod (call_frame_t *frame,
	      xlator_t *xl,
	      const char *path,
	      mode_t mode)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND (frame,
		stripe_chmod_cbk,
		trav->xlator,
		trav->xlator->fops->chmod,
		path,
		mode);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_chown_cbk (call_frame_t *frame,
		  call_frame_t *prev_frame,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct stat *stbuf)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, stbuf);
  }
  return 0;
}

static int32_t
stripe_chown (call_frame_t *frame,
	      xlator_t *xl,
	      const char *path,
	      uid_t uid,
	      gid_t gid)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;

  LOCK_INIT (&frame->mutex);
  
  while (trav) {
    STACK_WIND (frame,
		stripe_chown_cbk,
		trav->xlator,
		trav->xlator->fops->chown,
		path,
		uid,
		gid);
    trav = trav->next;
  }
  return 0;
}



/* access */
static int32_t
stripe_access (call_frame_t *frame,
	       xlator_t *xl,
	       const char *path,
	       mode_t mode)
{
  STACK_UNWIND (frame, -1, ENOSYS);
  return 0;
}

static int32_t
stripe_lock_cbk (call_frame_t *frame,
		 call_frame_t *prev_frame,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno)
{
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t
stripe_lock (call_frame_t *frame,
	     xlator_t *xl,
	     const char *path)
{
  xlator_t *stripexl = xl->children->xlator;

  STACK_WIND (frame,
	      stripe_lock_cbk,
	      stripexl,
	      stripexl->mops->lock,
	      path);
  return 0;
}

static int32_t
stripe_unlock_cbk (call_frame_t *frame,
		   call_frame_t *prev_frame,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno)
{
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t
stripe_unlock (call_frame_t *frame,
	       xlator_t *xl,
	       const char *path)
{
  xlator_t *stripexl = xl->children->xlator;
  STACK_WIND (frame,
	      stripe_unlock_cbk,
	      stripexl,
	      stripexl->mops->unlock,
	      path);
  return 0;
}


static int32_t
stripe_stats_cbk (call_frame_t *frame,
		  call_frame_t *prev_frame,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct xlator_stats *stats)
{
  stripe_local_t *local = frame->local;
  int32_t callcnt;
  LOCK(&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }
  if (op_ret == 0) {
    LOCK (&frame->mutex);
    if (local->op_ret == -2) {
      /* This is to make sure this is the frist time */
      local->stats = *stats;
      local->op_ret = 0;
    } else {
      local->stats.nr_files += stats->nr_files;
      local->stats.free_disk += stats->free_disk;
      local->stats.disk_usage += stats->disk_usage;
      local->stats.nr_clients += stats->nr_clients;
    }
    UNLOCK (&frame->mutex);
  }
   
  if (callcnt == ((stripe_private_t*)xl->private)->child_count) {
    LOCK_DESTROY(&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stats);
  }

  return 0;
}

static int32_t
stripe_stats (call_frame_t *frame,
	      struct xlator *xl,
	      int32_t flags)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  local->op_ret = -2; /* ugly */
  LOCK_INIT (&frame->mutex);
  
  while (trav) {
    STACK_WIND (frame,
		stripe_stats_cbk,
		trav->xlator,
		trav->xlator->mops->stats,
		flags);
    trav = trav->next;
  }
  return 0;
}

int32_t
init (xlator_t *xl)
{
  stripe_private_t *priv = calloc (1, sizeof (stripe_private_t));
  xlator_list_t *trav = xl->children;
  int count = 0;
  
  while (trav) {
    count++;
    trav = trav->next;
  }

  priv->child_count = count;

  /* option stripe-pattern *avi:1GB,*pdf:4096 */
  data_t *stripe_data = dict_get (xl->options, "block-size");
  if (!stripe_data) {
    gf_log ("stripe-init", 
	    GF_LOG_WARNING,
	    "no stripe pattern specified, no benefits of striping");
  } else {
    char *tmp_str;
    struct stripe_options *temp_stripeopt;
    char *stripe_str = strtok_r (stripe_data->data, ",", &tmp_str);
    while (stripe_str) {
      char *tmp_str1;
      char *dup_str = strdup (stripe_str);
      struct stripe_options *stripe_opt = calloc (1, sizeof (struct stripe_options));
      char *pattern =  strtok_r (dup_str, ":", &tmp_str1);
      char *num = strtok_r (NULL, ":", &tmp_str1);
      memcpy (stripe_opt->path_pattern, pattern, strlen (pattern));
      if (num) 
	stripe_opt->block_size = gf_str_to_long_long (num);
      else {
	stripe_opt->block_size = gf_str_to_long_long ("128KB");
      }
      gf_log ("init", 
	      GF_LOG_DEBUG, 
	      "stripe block size : pattern %s : size %d", 
	      stripe_opt->path_pattern, 
	      stripe_opt->block_size);
      if (!priv->pattern) {
	priv->pattern = stripe_opt;
      } else {
	temp_stripeopt = priv->pattern;
	while (temp_stripeopt->next)
	  temp_stripeopt = temp_stripeopt->next;
	temp_stripeopt->next = stripe_opt;
      }
      stripe_str = strtok_r (NULL, ",", &tmp_str);
    }
  }
  xl->private = priv;
  
  return 0;
} 

void 
fini (xlator_t *xl)
{
  struct stripe_options *prev, *trav = ((stripe_private_t *)xl->private)->pattern;
  while (trav) {
    prev = trav;
    trav = trav->next;
    free (prev);
  }
  free (xl->private);
  return;
}


struct xlator_fops fops = {
  .getattr     = stripe_getattr,
  .unlink      = stripe_unlink,
  .symlink     = stripe_symlink,
  .rename      = stripe_rename,
  .link        = stripe_link,
  .chmod       = stripe_chmod,
  .chown       = stripe_chown,
  .truncate    = stripe_truncate,
  .utimes      = stripe_utimes,
  .create      = stripe_create,
  .open        = stripe_open,
  .readv       = stripe_readv,
  .writev      = stripe_writev,
  .statfs      = stripe_statfs,
  .flush       = stripe_flush,
  .release     = stripe_release,
  .fsync       = stripe_fsync,
  .setxattr    = stripe_setxattr,
  .getxattr    = stripe_getxattr,
  .listxattr   = stripe_listxattr,
  .removexattr = stripe_removexattr,
  .access      = stripe_access,
  .ftruncate   = stripe_ftruncate,
  .fgetattr    = stripe_fgetattr,
  .readdir     = stripe_readdir,
  .readlink    = stripe_readlink,
  .mkdir       = stripe_mkdir,
  .rmdir       = stripe_rmdir,
  .lk          = stripe_lk,
};

struct xlator_mops mops = {
  .stats  = stripe_stats,
  .lock   = stripe_lock,
  .unlock = stripe_unlock,
};
