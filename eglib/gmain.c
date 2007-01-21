#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <malloc.h>
#include <string.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

#include <gmain.h>

struct timeout {
	guint id;
	guint interval;
	struct timeval expiration;
	gpointer data;
	GSourceFunc function;
};

struct _GIOChannel {
	int fd;
	int ref_count;
	gboolean closed;
	gboolean close_on_unref;
};

struct child_watch {
	guint id;
	GPid pid;
	GChildWatchFunc function;
	gpointer user_data;
};

struct _GMainContext {
	guint next_id;
	glong next_timeout;

	GSList *timeouts;
	GSList *proc_timeouts;
	gboolean timeout_lock;

	GSList *io_watches;
	GSList *proc_io_watches;
	gboolean io_lock;

	GSList *child_watches;
	GSList *proc_child_watches;
	gboolean child_lock;
};

struct _GMainLoop {
	gboolean is_running;
	GMainContext *context;
};

GIOError g_io_channel_read(GIOChannel *channel, gchar *buf, gsize count, gsize *bytes_read)
{
	int fd = channel->fd;
	gssize result;

	if (channel->closed)
		return G_IO_STATUS_ERROR;

	/* At least according to the Debian manpage for read */
	if (count > SSIZE_MAX)
		count = SSIZE_MAX;

retry:
	result = read (fd, buf, count);

	if (result < 0) {
		*bytes_read = 0;

		switch (errno) {
#ifdef EINTR
		case EINTR:
			goto retry;
#endif
#ifdef EAGAIN
		case EAGAIN:
			return G_IO_STATUS_AGAIN;
#endif
		default:
			return G_IO_STATUS_ERROR;
		}
	}

	*bytes_read = result;

	return (result > 0) ? G_IO_STATUS_NORMAL : G_IO_STATUS_EOF;
}

GIOError g_io_channel_write(GIOChannel *channel, const gchar *buf, gsize count,
				gsize *bytes_written)
{
	int fd = channel->fd;
	gssize result;

	if (channel->closed)
		return G_IO_STATUS_ERROR;

	/* At least according to the Debian manpage for read */
	if (count > SSIZE_MAX)
		count = SSIZE_MAX;

retry:
	result = write(fd, buf, count);

	if (result < 0) {
		*bytes_written = 0;

		switch (errno) {
#ifdef EINTR
		case EINTR:
			goto retry;
#endif
#ifdef EAGAIN
		case EAGAIN:
			return G_IO_STATUS_AGAIN;
#endif
		default:
			return G_IO_STATUS_ERROR;
		}
	}

	*bytes_written = result;

	return (result > 0) ? G_IO_STATUS_NORMAL : G_IO_STATUS_EOF;
}

void g_io_channel_close(GIOChannel *channel)
{
	if (!channel || channel->closed)
		return;

	close(channel->fd);

	channel->closed = TRUE;
}

void g_io_channel_unref(GIOChannel *channel)
{
	if (--channel->ref_count > 0)
		return;

	if (!channel)
		return;

	if (channel->close_on_unref && channel->fd >= 0)
		g_io_channel_close(channel);

	g_free(channel);
}

GIOChannel *g_io_channel_ref(GIOChannel *channel)
{
	channel->ref_count++;
	return channel;
}

GIOChannel *g_io_channel_unix_new(int fd)
{
	GIOChannel *channel;

	channel = g_new0(GIOChannel, 1);

	channel->fd = fd;
	channel->ref_count = 1;

	return channel;
}

void g_io_channel_set_close_on_unref(GIOChannel *channel, gboolean do_close)
{
	channel->close_on_unref = do_close;
}

gint g_io_channel_unix_get_fd(GIOChannel *channel)
{
	if (channel->closed)
		return -1;

	return channel->fd;
}

struct io_watch {
	guint id;
	GIOChannel *channel;
	gint priority;
	GIOCondition condition;
	short *revents;
	GIOFunc func;
	gpointer user_data;
	GDestroyNotify destroy;
};

static GMainContext *default_context = NULL;

static void watch_free(struct io_watch *watch)
{
	if (watch->destroy)
		watch->destroy(watch->user_data);
	g_io_channel_unref(watch->channel);
	g_free(watch);
}

static GMainContext *g_main_context_default()
{
	if (default_context)
		return default_context;

	default_context = g_new0(GMainContext, 1);

	default_context->next_timeout = -1;
	default_context->next_id = 1;

	return default_context;
}

static gboolean g_io_remove_watch(GMainContext *context, guint id)
{
	GSList *l;
	struct io_watch *w;

	for (l = context->io_watches; l != NULL; l = l->next) {
		w = l->data;	

		if (w->id != id)
			continue;

		context->io_watches = g_slist_remove(context->io_watches, w);
		watch_free(w);

		return TRUE;
	}

	for (l = context->proc_io_watches; l != NULL; l = l->next) {
		w = l->data;	

		if (w->id != id)
			continue;

		context->proc_io_watches = g_slist_remove(context->proc_io_watches, w);
		watch_free(w);

		return TRUE;
	}

	return FALSE;
}

static gboolean g_timeout_remove(GMainContext *context, const guint id)
{
	GSList *l;
	struct timeout *t;

	l = context->timeouts;

	while (l) {
		t = l->data;
		l = l->next;

		if (t->id != id)
			continue;

		context->timeouts = g_slist_remove(context->timeouts, t);
		g_free(t);

		return TRUE;
	}

	l = context->proc_timeouts;

	while (l) {
		t = l->data;
		l = l->next;

		if (t->id != id)
			continue;

		context->proc_timeouts = g_slist_remove(context->proc_timeouts, t);
		g_free(t);

		return TRUE;
	}

	return FALSE;
}

int watch_prio_cmp(struct io_watch *w1, struct io_watch *w2)
{
	return w1->priority - w2->priority;
}

#define watch_list_add(l, w) g_slist_insert_sorted((l), (w), (GCompareFunc) watch_prio_cmp)

guint g_io_add_watch_full(GIOChannel *channel, gint priority,
				GIOCondition condition, GIOFunc func,
				gpointer user_data, GDestroyNotify notify)
{
	struct io_watch *watch;
	GMainContext *context = g_main_context_default();

	watch = g_new(struct io_watch, 1);

	watch->id = context->next_id++;
	watch->channel = g_io_channel_ref(channel);
	watch->priority = priority;
	watch->condition = condition;
	watch->func = func;
	watch->user_data = user_data;
	watch->destroy = notify;

	if (context->io_lock)
		context->proc_io_watches = watch_list_add(context->proc_io_watches, watch);
	else
		context->io_watches = watch_list_add(context->io_watches, watch);

	return watch->id;
}

guint g_io_add_watch(GIOChannel *channel, GIOCondition condition,
					GIOFunc func, gpointer user_data)
{
	return g_io_add_watch_full(channel, 0, condition,
						func, user_data, NULL);
}

GMainLoop *g_main_loop_new(GMainContext *context, gboolean is_running)
{
	GMainLoop *ml;

	if (!context)
		context = g_main_context_default();

	ml = g_new0(GMainLoop, 1);

	ml->context = context;
	ml->is_running = is_running;

	return ml;
}

static void timeout_handlers_prepare(GMainContext *context)
{
	GSList *l;
	struct timeval tv;
	glong msec, timeout = LONG_MAX;

	gettimeofday(&tv, NULL);

	for (l = context->timeouts; l != NULL; l = l->next) {
		struct timeout *t = l->data;

		/* calculate the remainning time */
		msec = (t->expiration.tv_sec - tv.tv_sec) * 1000 +
				(t->expiration.tv_usec - tv.tv_usec) / 1000;
		if (msec < 0)
			msec = 0;

		timeout = MIN_TIMEOUT(timeout, msec);
	}

	/* set to min value found or NO timeout */
	context->next_timeout = (timeout != LONG_MAX ? timeout : -1);
}

static int ptr_cmp(const void *t1, const void *t2)
{
	return t1 - t2;
}

static void timeout_handlers_check(GMainContext *context)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	context->timeout_lock = TRUE;

	while (context->timeouts) {
		struct timeout *t = context->timeouts->data;
		glong secs, msecs;
		gboolean ret;

		if (timercmp(&tv, &t->expiration, <)) {
			context->timeouts = g_slist_remove(context->timeouts, t);
			context->proc_timeouts = g_slist_append(context->proc_timeouts, t);
			continue;
		}

		ret = t->function(t->data);

		/* Check if the handler was removed/freed by the callback
		 * function */
		if (!g_slist_find_custom(context->timeouts, t, ptr_cmp))
			continue;

		context->timeouts = g_slist_remove(context->timeouts, t);

		if (!ret) {
			g_free(t);
			continue;
		}

		/* update the next expiration time */
		secs = t->interval / 1000;
		msecs = t->interval - secs * 1000;

		t->expiration.tv_sec = tv.tv_sec + secs;
		t->expiration.tv_usec = tv.tv_usec + msecs * 1000;
		if (t->expiration.tv_usec >= 1000000) {
			t->expiration.tv_usec -= 1000000;
			t->expiration.tv_sec++;
		}

		context->proc_timeouts = g_slist_append(context->proc_timeouts, t);
	}

	context->timeouts = context->proc_timeouts;
	context->proc_timeouts = NULL;
	context->timeout_lock = FALSE;
}

void g_main_loop_run(GMainLoop *loop)
{
	int open_max = sysconf(_SC_OPEN_MAX);
	struct pollfd *ufds;
	GMainContext *context = loop->context;

	ufds = g_new(struct pollfd, open_max);

	loop->is_running = TRUE;

	while (loop->is_running) {
		int nfds;
		GSList *l;
		struct io_watch *w;

		for (nfds = 0, l = context->io_watches; l != NULL; l = l->next, nfds++) {
			w = l->data;
			ufds[nfds].fd = w->channel->fd;
			ufds[nfds].events = w->condition;
			ufds[nfds].revents = 0;
			w->revents = &ufds[nfds].revents;
		}

		/* calculate the next timeout */
		timeout_handlers_prepare(context);

		if (poll(ufds, nfds, context->next_timeout) < 0)
			continue;

		context->io_lock = TRUE;

		while (context->io_watches) {
			gboolean ret;

			w = context->io_watches->data;

			if (!*w->revents) {
				context->io_watches = g_slist_remove(context->io_watches, w);
				context->proc_io_watches = watch_list_add(context->proc_io_watches, w);
				continue;
			}

			ret = w->func(w->channel, *w->revents, w->user_data);

			/* Check if the watch was removed/freed by the callback
			 * function */
			if (!g_slist_find_custom(context->io_watches, w, ptr_cmp))
				continue;

			context->io_watches = g_slist_remove(context->io_watches, w);

			if (!ret) {
				watch_free(w);
				continue;
			}

			context->proc_io_watches = watch_list_add(context->proc_io_watches, w);
		}

		context->io_watches = context->proc_io_watches;
		context->proc_io_watches = NULL;
		context->io_lock = FALSE;

		/* check expired timers */
		timeout_handlers_check(loop->context);
	}

	g_free(ufds);
}

void g_main_loop_quit(GMainLoop *loop)
{
	loop->is_running = FALSE;
}

void g_main_loop_unref(GMainLoop *loop)
{
	if (!loop->context)
		return;

	g_slist_foreach(loop->context->io_watches, (GFunc)watch_free, NULL);
	g_slist_free(loop->context->io_watches);

	g_slist_foreach(loop->context->timeouts, (GFunc)g_free, NULL);
	g_slist_free(loop->context->timeouts);

	g_free(loop->context);
	loop->context = NULL;
}

guint g_timeout_add(guint interval, GSourceFunc function, gpointer data)
{
	GMainContext *context = g_main_context_default();
	struct timeval tv;
	guint secs;
	guint msecs;
	struct timeout *t;

	t = g_new0(struct timeout, 1);

	t->interval = interval;
	t->function = function;
	t->data = data;

	gettimeofday(&tv, NULL);

	secs = interval /1000;
	msecs = interval - secs * 1000;

	t->expiration.tv_sec = tv.tv_sec + secs;
	t->expiration.tv_usec = tv.tv_usec + msecs * 1000;

	if (t->expiration.tv_usec >= 1000000) {
		t->expiration.tv_usec -= 1000000;
		t->expiration.tv_sec++;
	}

	/* attach the timeout the default context */
	t->id = context->next_id++;

	if (context->timeout_lock)
		context->proc_timeouts = g_slist_prepend(context->proc_timeouts, t);
	else
		context->timeouts = g_slist_prepend(context->timeouts, t);

	return t->id;
}

/* GError */
void g_error_free(GError *err)
{
	g_free(err->message);
	g_free(err);
}

/* Spawning related functions */

static int child_watch_pipe[2] = { -1, -1 };

static void sigchld_handler(int signal)
{
	int ret;
	ret = write(child_watch_pipe[1], "B", 1);
}

static gboolean child_watch_remove(GMainContext *context, guint id)
{
	GSList *l;
	struct child_watch *w;

	for (l = context->child_watches; l != NULL; l = l->next) {
		w = l->data;

		if (w->id != id)
			continue;

		context->child_watches =
			g_slist_remove(context->child_watches, w);
		g_free(w);

		return TRUE;
	}

	for (l = context->proc_child_watches; l != NULL; l = l->next) {
		w = l->data;

		if (w->id != id)
			continue;

		context->proc_child_watches =
			g_slist_remove(context->proc_child_watches, w);
		g_free(w);

		return TRUE;
	}


	return FALSE;
}

static gboolean child_watch(GIOChannel *io, GIOCondition cond, gpointer user_data)
{
	int ret;
	char b[20];
	GMainContext *context = g_main_context_default();

	ret = read(child_watch_pipe[0], b, 20);

	context->child_lock = TRUE;

	while (context->child_watches) {
		gint status;
		struct child_watch *w = context->child_watches->data;

		if (waitpid(w->pid, &status, WNOHANG) <= 0) {
			context->child_watches =
				g_slist_remove(context->child_watches, w);
			context->proc_child_watches =
				watch_list_add(context->proc_child_watches, w);
			continue;
		}

		w->function(w->pid, status, w->user_data);

		/* Check if the callback already removed us */
		if (!g_slist_find(context->child_watches, w))
			continue;

		context->child_watches = g_slist_remove(context->child_watches, w);
		g_free(w);
	}

	context->child_watches = context->proc_child_watches;
	context->proc_child_watches = NULL;
	context->child_lock = FALSE;

	return TRUE;
}

static void init_child_pipe(void)
{  
	struct sigaction action;
	GIOChannel *io;

	if (pipe(child_watch_pipe) < 0) {
		fprintf(stderr, "Unable to initialize child watch pipe: %s (%d)\n",
				strerror(errno), errno);
		abort();
	}

	fcntl(child_watch_pipe[1], F_SETFL,
			O_NONBLOCK | fcntl(child_watch_pipe[1], F_GETFL));

	action.sa_handler = sigchld_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_NOCLDSTOP;
	sigaction(SIGCHLD, &action, NULL);

	io = g_io_channel_unix_new(child_watch_pipe[0]);
	g_io_add_watch(io, G_IO_IN, child_watch, NULL);
	g_io_channel_unref(io);
}

gboolean g_spawn_async(const gchar *working_directory,
			gchar **argv, gchar **envp,
			GSpawnFlags flags,
			GSpawnChildSetupFunc child_setup,
			gpointer user_data,
			GPid *child_pid,
			GError **error)
{
	if (child_watch_pipe[0] < 0)
		init_child_pipe();

	return FALSE;
}

void g_spawn_close_pid(GPid pid)
{
	return;
}

guint g_child_watch_add(GPid pid, GChildWatchFunc func, gpointer user_data)
{
	struct child_watch *w;
	GMainContext *context = g_main_context_default();

	if (child_watch_pipe[0] < 0)
		init_child_pipe();

	w = g_new(struct child_watch, 1);

	w->id = context->next_id++;
	w->pid = pid;
	w->function = func;
	w->user_data = user_data;

	if (context->child_lock)
		context->proc_child_watches =
			watch_list_add(context->proc_child_watches, w);
	else
		context->child_watches =
			watch_list_add(context->child_watches, w);

	return w->id;
}

gboolean g_source_remove(guint tag)
{
	GMainContext *context = g_main_context_default();

	if (g_io_remove_watch(context, tag))
		return TRUE;

	if (g_timeout_remove(context, tag))
		return TRUE;

	if (child_watch_remove(context, tag))
		return TRUE;

	return FALSE;
}

/* UTF-8 Validation: approximate copy/paste from glib2. */

#define UNICODE_VALID(c)			\
	((c) < 0x110000 &&			\
	(((c) & 0xFFFFF800) != 0xD800) &&	\
	((c) < 0xFDD0 || (c) > 0xFDEF) &&	\
	((c) & 0xFFFE) != 0xFFFE)

#define CONTINUATION_CHAR(c, val)				\
	do {							\
  		if (((c) & 0xc0) != 0x80) /* 10xxxxxx */	\
  			goto failed;				\
  		(val) <<= 6;					\
  		(val) |= (c) & 0x3f;				\
	} while (0)

#define INCREMENT_AND_CHECK_MAX(p, i, max_len)					\
	do {									\
		(i)++;								\
		if ((p)[(i)] == '\0' || ((max_len) >= 0 && (i) >= (max_len)))	\
			goto failed;						\
	} while (0)
				

gboolean g_utf8_validate(const gchar *str, gssize max_len, const gchar **end)
{
	unsigned long val, min, i;
	const unsigned char *p, *last;

	min = val = 0;

	for (p = (unsigned char *) str, i = 0; p[i]; i++) {
		if (max_len >= 0 && i >= max_len)
			break;

		if (p[i] < 128)
			continue;

		last = &p[i];

		if ((p[i] & 0xe0) == 0xc0) { /* 110xxxxx */
			if ((p[i] & 0x1e) == 0)
				goto failed;
			INCREMENT_AND_CHECK_MAX(p, i, max_len);
			if ((p[i] & 0xc0) != 0x80)
				goto failed; /* 10xxxxxx */
		} else {
			if ((p[i] & 0xf0) == 0xe0) {
				/* 1110xxxx */
				min = (1 << 11);
				val = p[i] & 0x0f;
				goto two_remaining;
			} else if ((p[i] & 0xf8) == 0xf0) {
				/* 11110xxx */
				min = (1 << 16);
				val = p[i] & 0x07;
			} else
				goto failed;

			INCREMENT_AND_CHECK_MAX(p, i, max_len);
			CONTINUATION_CHAR(p[i], val);
two_remaining:
			INCREMENT_AND_CHECK_MAX(p, i, max_len);
			CONTINUATION_CHAR(p[i], val);

			INCREMENT_AND_CHECK_MAX(p, i, max_len);
			CONTINUATION_CHAR(p[i], val);

			if (val < min || !UNICODE_VALID(val))
				goto failed;
		} 
	}

	if (end)
		*end = (const gchar *) &p[i];

	return TRUE;

failed:
	if (end)
		*end = (const gchar *) last;

	return FALSE;
}

/* GSList functions */

GSList *g_slist_append(GSList *list, void *data)
{
	GSList *entry, *tail;

	entry = g_new(GSList, 1);

	entry->data = data;
	entry->next = NULL;

	if (!list)
		return entry;

	/* Find the end of the list */
	for (tail = list; tail->next; tail = tail->next);

	tail->next = entry;

	return list;
}

GSList *g_slist_prepend(GSList *list, void *data)
{
	GSList *entry;

	entry = g_new(GSList, 1);

	entry->data = data;
	entry->next = list;

	return entry;
}

GSList *g_slist_insert_sorted(GSList *list, void *data, GCompareFunc cmp_func)
{
	GSList *tmp, *prev, *entry;
	int cmp;

	entry = g_new(GSList, 1);

	entry->data = data;
	entry->next = NULL;

	if (!list)
		return entry;

	prev = NULL;
	tmp = list;

	cmp = cmp_func(data, tmp->data);

	while (tmp->next && cmp > 0) {
		prev = tmp;
		tmp = tmp->next;

		cmp = cmp_func(data, tmp->data);
	}

	if (!tmp->next && cmp > 0) {
		tmp->next = entry;
		return list;
	}

	if (prev) {
		prev->next = entry;
		entry->next = tmp;
		return list;
	} else {
		entry->next = list;
		return entry;
	}
}

GSList *g_slist_remove(GSList *list, void *data)
{
	GSList *l, *next, *prev = NULL, *match = NULL;

	if (!list)
		return NULL;

	for (l = list; l != NULL; l = l->next) {
		if (l->data == data) {
			match = l;
			break;
		}
		prev = l;
	}

	if (!match)
		return list;

	next = match->next;

	g_free(match);

	/* If the head was removed, return the next element */
	if (!prev)
		return next;

	prev->next = next;

	return list;
}

GSList *g_slist_find(GSList *list, gconstpointer data)
{
	GSList *l;

	for (l = list; l != NULL; l = l->next) {
		if (l->data == data)
			return l;
	}

	return NULL;
}

GSList *g_slist_find_custom(GSList *list, const void *data,
			GCompareFunc cmp_func)
{
	GSList *l;

	for (l = list; l != NULL; l = l->next) {
		if (!cmp_func(l->data, data))
			return l;
	}

	return NULL;
}

static GSList *g_slist_sort_merge(GSList *l1, GSList *l2,
					GCompareFunc cmp_func)
{
	GSList list, *l;
	int cmp;

	l = &list;

	while (l1 && l2) {
		cmp = cmp_func(l1->data, l2->data);

		if (cmp <= 0) {
			l = l->next = l1;
			l1 = l1->next;
		} else {
			l = l->next = l2;
			l2 = l2->next;
		}
	}

	l->next = l1 ? l1 : l2;

	return list.next;
}

GSList *g_slist_sort(GSList *list, GCompareFunc cmp_func)
{
	GSList *l1, *l2;

	if (!list || !list->next) 
		return list;

	l1 = list; 
	l2 = list->next;

	while ((l2 = l2->next) != NULL) {
		if ((l2 = l2->next) == NULL) 
			break;
		l1 = l1->next;
	}

	l2 = l1->next; 
	l1->next = NULL;

	return g_slist_sort_merge(g_slist_sort(list, cmp_func),
				g_slist_sort(l2, cmp_func), cmp_func);
}

int g_slist_length(GSList *list)
{
	int len;

	for (len = 0; list != NULL; list = list->next)
		len++;

	return len;
}

void g_slist_foreach(GSList *list, GFunc func, void *user_data)
{
	while (list) {
		GSList *next = list->next;
		func(list->data, user_data);
		list = next;
	}
}

void g_slist_free(GSList *list)
{
	GSList *l, *next;

	for (l = list; l != NULL; l = next) {
		next = l->next;
		g_free(l);
	}
}

/* Memory allocation functions */

gpointer g_malloc(gulong n_bytes)
{
	gpointer mem;

	if (!n_bytes)
		return NULL;

	mem = malloc((size_t) n_bytes);
	if (!mem) {
		fprintf(stderr, "g_malloc: failed to allocate %lu bytes",
				n_bytes);
		abort();
	}

	return mem;
}

gpointer g_malloc0(gulong n_bytes)
{
	gpointer mem;

	if (!n_bytes)
		return NULL;

	mem = g_malloc(n_bytes);

	memset(mem, 0, (size_t) n_bytes);

	return mem;
}

gpointer g_try_malloc(gulong n_bytes)
{
	if (!n_bytes)
		return NULL;

	return malloc((size_t) n_bytes);
}

gpointer g_try_malloc0(gulong n_bytes)
{
	gpointer mem;

	mem = g_try_malloc(n_bytes);
	if (mem)
		memset(mem, 0, (size_t) n_bytes);

	return mem;
}

void g_free(gpointer mem)
{
	if (mem)
		free(mem);
}

gchar *g_strdup(const gchar *str)
{
	gchar *s;

	if (!str)
		return NULL;

	s = strdup(str);
	if (!s) {
		fprintf(stderr, "strdup: failed to allocate new string");
		abort();
	}

	return s;
}

gchar *g_strdup_printf(const gchar *format, ...)
{
	gchar str[1024];
	va_list ap;

	memset(str, 0, sizeof(str));

	va_start(ap, format);

	vsnprintf(str, sizeof(str) - 1, format, ap);

	va_end(ap);

	return g_strdup(str);
}

void g_strfreev(gchar **str_array)
{
	int i;

	if (!str_array)
		return;
	
	for (i = 0; str_array[i] != NULL; i++)
		g_free(str_array[i]);

	g_free(str_array);
}

/* g_shell_* */

gboolean g_shell_parse_argv(const gchar *command_line,
				gint *argcp,
				gchar ***argvp,
				GError **error)
{
	/* Not implemented */
	return FALSE;
}

/* GKeyFile */

typedef gpointer GHashTable;

typedef struct _GKeyFileGroup GKeyFileGroup;

struct _GKeyFile {
	GSList *groups;

	GKeyFileGroup *start_group;
	GKeyFileGroup *current_group;

	/* Holds up to one line of not-yet-parsed data */
	gchar *parse_buffer;

	/* Used for sizing the output buffer during serialization */
	gsize approximate_size;

	gchar list_separator;

	GKeyFileFlags flags;
};

typedef struct _GKeyFileKeyValuePair GKeyFileKeyValuePair;

struct _GKeyFileGroup {
	/* NULL for above first group (which will be comments) */
	const gchar *name;

	/* Special comment that is stuck to the top of a group */
	GKeyFileKeyValuePair *comment;

	GSList *key_value_pairs;

	/* Used in parallel with key_value_pairs for
	 * increased lookup performance
	 */
	GHashTable *lookup_map;
};

struct _GKeyFileKeyValuePair {
	gchar *key;  /* NULL for comments */
	gchar *value;
};

GKeyFile *g_key_file_new(void)
{
	/* Not implemented */
	return NULL;
}

void g_key_file_free(GKeyFile *key_file)
{
	/* Not implemented fully */
	g_free(key_file);
}

gboolean g_key_file_load_from_file(GKeyFile *key_file,
				const gchar *file,
				GKeyFileFlags flags,
				GError **error)
{
	/* Not implemented */
	return FALSE;
}

gchar *g_key_file_get_string(GKeyFile *key_file,
				const gchar *group_name,
				const gchar *key,
				GError **error)
{
	/* Not implemented */
	return NULL;
}

gboolean g_key_file_get_boolean(GKeyFile *key_file,
				const gchar *group_name,
				const gchar *key,
				GError **error)
{
	/* Not implemented */
	return FALSE;
}

