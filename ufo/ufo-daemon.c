/*
 * Copyright (C) 2011-2013 Karlsruhe Institute of Technology
 *
 * This file is part of Ufo.
 *
 * This library is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

#include "config.h"

#ifdef WITH_MPI
#include <mpi.h>
#include <ufo/ufo-mpi-messenger.h>
#endif

#ifdef WITH_ZMQ
#include <ufo/ufo-zmq-messenger.h>
#endif

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <ufo/ufo-daemon.h>
#include <ufo/ufo-dummy-task.h>
#include <ufo/ufo-input-task.h>
#include <ufo/ufo-output-task.h>
#include <ufo/ufo-plugin-manager.h>
#include <ufo/ufo-scheduler.h>
#include <ufo/ufo-task-graph.h>
#include <ufo/ufo-messenger-iface.h>

G_DEFINE_TYPE (UfoDaemon, ufo_daemon, G_TYPE_OBJECT)

#define UFO_DAEMON_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), UFO_TYPE_DAEMON, UfoDaemonPrivate))

struct _UfoDaemonPrivate {
    UfoPluginManager *manager;
    UfoResources *resources;
    UfoTaskGraph *task_graph;
    UfoBaseScheduler *scheduler;
    GThread *scheduler_thread;
    gpointer socket;
    UfoNode *input_task;
    UfoNode *output_task;
    UfoBuffer *input;
    cl_context context;
    gchar *listen_address;
    GThread *thread;
    GMutex *startstop_lock;
    GMutex *started_lock;
    GMutex *stopped_lock;
    gboolean has_started;
    gboolean has_stopped;
    GCond *started_cond;
    GCond *stopped_cond;
    UfoMessenger *messenger;
};

static gpointer run_scheduler (UfoDaemon *daemon);
static void handle_get_num_devices (UfoDaemon *, UfoMessage *);
static void handle_stream_json (UfoDaemon *, UfoMessage *);
static void handle_replicate_json (UfoDaemon *, UfoMessage *);
static void handle_get_structure (UfoDaemon *, UfoMessage *);
static void handle_send_inputs (UfoDaemon *, UfoMessage *);
static void handle_get_requisition (UfoDaemon *, UfoMessage *);
static void handle_get_result (UfoDaemon *, UfoMessage *);
static void handle_cleanup (UfoDaemon *, UfoMessage *);
static void handle_terminate (UfoDaemon *, UfoMessage *);

typedef void (*RequestHandler) (UfoDaemon *daemon, UfoMessage *request);

static RequestHandler handlers[] = {
    handle_stream_json,
    handle_replicate_json,
    handle_get_num_devices,
    handle_get_structure,
    handle_get_requisition,
    handle_send_inputs,
    handle_get_result,
    handle_cleanup,
    handle_terminate,
};

UfoDaemon *
ufo_daemon_new (const gchar *listen_address)
{
    UfoDaemonPrivate *priv;
    UfoDaemon *daemon;

    g_return_val_if_fail (listen_address != NULL, NULL);

    daemon = UFO_DAEMON (g_object_new (UFO_TYPE_DAEMON, NULL));
    priv = UFO_DAEMON_GET_PRIVATE (daemon);

    priv->listen_address = g_strdup (listen_address);
    priv->manager = ufo_plugin_manager_new ();

#ifdef WITH_MPI
    priv->messenger = UFO_MESSENGER (ufo_mpi_messenger_new ());
#elif WITH_ZMQ
    priv->messenger = UFO_MESSENGER (ufo_zmq_messenger_new ());
#endif

    return daemon;
}

static void
handle_get_num_devices (UfoDaemon *daemon, UfoMessage *request)
{
    UfoDaemonPrivate *priv;
    UfoMessage *reply;
    cl_uint num_devices;
    cl_context context;

    priv = UFO_DAEMON_GET_PRIVATE (daemon);
    reply = ufo_message_new (UFO_MESSAGE_ACK, sizeof (guint16));
    context = ufo_resources_get_context (priv->resources);

    UFO_RESOURCES_CHECK_CLERR (clGetContextInfo (context, CL_CONTEXT_NUM_DEVICES, sizeof (cl_uint), &num_devices, NULL));

    *(guint16 *) reply->data = (guint16) num_devices;

    ufo_messenger_send_blocking (priv->messenger, reply, 0);
    ufo_message_free (reply);
}

static UfoNode *
remove_dummy_if_present (UfoGraph *graph, UfoNode *first)
{
    UfoNode *real = first;

    if (UFO_IS_DUMMY_TASK (first)) {
        UfoNode *dummy;
        GList *successors;

        dummy = first;
        successors = ufo_graph_get_successors (graph, dummy);
        g_assert (g_list_length (successors) == 1);
        real = UFO_NODE (successors->data);
        g_list_free (successors);
        ufo_graph_remove_edge (graph, dummy, real);
    }

    return real;
}

static gchar *
read_json (UfoDaemon *daemon, UfoMessage *message)
{
    gchar *json;

    json = g_malloc0 (message->data_size + 1);
    memcpy (json, message->data, message->data_size);

    return json;
}

static void
handle_replicate_json (UfoDaemon *daemon, UfoMessage *request)
{
    UfoDaemonPrivate *priv = UFO_DAEMON_GET_PRIVATE (daemon);
    gchar *json;
    UfoTaskGraph *graph;
    GError *error = NULL;

    json = read_json (daemon, request);

    /* send ack */
    UfoMessage *reply = ufo_message_new (UFO_MESSAGE_ACK, 0);
    ufo_messenger_send_blocking (priv->messenger, reply, NULL);
    ufo_message_free (reply);

    graph = UFO_TASK_GRAPH (ufo_task_graph_new ());
    ufo_task_graph_read_from_data (graph, priv->manager, json, &error);

    if (error != NULL) {
        g_printerr ("%s\n", error->message);
        goto replicate_json_free;
    }

    ufo_base_scheduler_run (priv->scheduler, graph, NULL);
    g_object_unref (priv->scheduler);

    priv->scheduler = ufo_scheduler_new ();

replicate_json_free:
    g_object_unref (graph);
    g_free (json);
}

static void
handle_stream_json (UfoDaemon *daemon, UfoMessage *request)
{
    UfoDaemonPrivate *priv;
    gchar *json;
    GList *roots;
    GList *leaves;
    UfoNode *first;
    UfoNode *last;
    GError *error = NULL;

    priv = UFO_DAEMON_GET_PRIVATE (daemon);
    json = read_json (daemon, request);

    /* send ack */
    UfoMessage *reply = ufo_message_new (UFO_MESSAGE_ACK, 0);
    ufo_messenger_send_blocking (priv->messenger, reply, NULL);
    ufo_message_free (reply);

    /* Setup local task graph */
    priv->task_graph = UFO_TASK_GRAPH (ufo_task_graph_new ());
    ufo_task_graph_read_from_data (priv->task_graph, priv->manager, json, &error);

    if (error != NULL) {
        g_printerr ("%s\n", error->message);
        /* Send error to master */
        return;
    }

    roots = ufo_graph_get_roots (UFO_GRAPH (priv->task_graph));
    g_assert (g_list_length (roots) == 1);

    leaves = ufo_graph_get_leaves (UFO_GRAPH (priv->task_graph));
    g_assert (g_list_length (leaves) == 1);

    first = UFO_NODE (g_list_nth_data (roots, 0));
    last = UFO_NODE (g_list_nth_data (leaves, 0));

    first = remove_dummy_if_present (UFO_GRAPH (priv->task_graph), first);

    priv->input_task = ufo_input_task_new ();
    priv->output_task = ufo_output_task_new (2);

    ufo_graph_connect_nodes (UFO_GRAPH (priv->task_graph), priv->input_task, first, GINT_TO_POINTER (0));
    ufo_graph_connect_nodes (UFO_GRAPH (priv->task_graph), last, priv->output_task, GINT_TO_POINTER (0));

    priv->scheduler_thread = g_thread_create ((GThreadFunc) run_scheduler, daemon, TRUE, NULL);
    g_free (json);
}

static void
handle_get_structure (UfoDaemon *daemon, UfoMessage *request)
{
    UfoDaemonPrivate *priv = UFO_DAEMON_GET_PRIVATE (daemon);
    UfoMessage *reply;

    /* TODO move into .h and share between daemon and remote-node */
    struct _Structure {
        guint16 n_inputs;
        guint16 n_dims;
    } message_data;

    /* TODO don't hardcode these */
    message_data.n_inputs = 1;
    message_data.n_dims = 2;

    reply = ufo_message_new (UFO_MESSAGE_ACK, sizeof (struct _Structure));
    *(struct _Structure *) (reply->data) = message_data;

    ufo_messenger_send_blocking (priv->messenger, reply, NULL);
    ufo_message_free (reply);
}

static void
handle_send_inputs (UfoDaemon *daemon, UfoMessage *request)
{
    UfoDaemonPrivate *priv;
    UfoRequisition requisition;
    gpointer context;

    priv = UFO_DAEMON_GET_PRIVATE (daemon);
    context = ufo_resources_get_context (priv->resources);

    struct Header {
        UfoRequisition requisition;
        guint64 buffer_size;
    };

    char *base = request->data;
    struct Header *header = (struct Header *) base;

    /* Receive buffer size */
    requisition = header->requisition;

    if (priv->input == NULL) {
        priv->input = ufo_buffer_new (&requisition, context);
    }
    else {
        if (ufo_buffer_cmp_dimensions (priv->input, &requisition))
            ufo_buffer_resize (priv->input, &requisition);
    }

    memcpy (ufo_buffer_get_host_array (priv->input, NULL),
            base + sizeof (struct Header),
            ufo_buffer_get_size (priv->input));

    ufo_input_task_release_input_buffer (UFO_INPUT_TASK (priv->input_task), priv->input);

    UfoMessage *reply = ufo_message_new (UFO_MESSAGE_ACK, 0);
    ufo_messenger_send_blocking (priv->messenger, reply, NULL);
    ufo_message_free (reply);
}

static void
handle_get_requisition (UfoDaemon *daemon, UfoMessage *request)
{
    UfoDaemonPrivate *priv = UFO_DAEMON_GET_PRIVATE (daemon);
    UfoRequisition requisition;

    /* We need to get the requisition from the last node */
    ufo_output_task_get_output_requisition (UFO_OUTPUT_TASK (priv->output_task),
                                            &requisition);

    UfoMessage *reply = ufo_message_new (UFO_MESSAGE_ACK, sizeof (UfoRequisition));
    memcpy (reply->data, &requisition, reply->data_size);
    ufo_messenger_send_blocking (priv->messenger, reply, NULL);
    ufo_message_free (reply);
}

static
void handle_get_result (UfoDaemon *daemon, UfoMessage *request)
{
    UfoDaemonPrivate *priv = UFO_DAEMON_GET_PRIVATE (daemon);
    UfoBuffer *buffer;
    gsize size;

    buffer = ufo_output_task_get_output_buffer (UFO_OUTPUT_TASK (priv->output_task));
    size = ufo_buffer_get_size (buffer);

    UfoMessage *reply = ufo_message_new (UFO_MESSAGE_ACK, size);
    memcpy (reply->data, ufo_buffer_get_host_array (buffer, NULL), size);
    ufo_messenger_send_blocking (priv->messenger, reply, NULL);
    ufo_output_task_release_output_buffer (UFO_OUTPUT_TASK (priv->output_task), buffer);
}

static void
unref_and_free (GObject **object)
{
    if (*object) {
        g_object_unref (*object);
        *object = NULL;
    }
}

static
void handle_cleanup (UfoDaemon *daemon, UfoMessage *request)
{
    UfoDaemonPrivate *priv = UFO_DAEMON_GET_PRIVATE (daemon);

    /*
     * We send the ACK early on, because we don't want to let the host wait for
     * actually cleaning up (and waiting some time to unref the input task).
     */
    UfoMessage *reply = ufo_message_new (UFO_MESSAGE_ACK, 0);
    ufo_messenger_send_blocking (priv->messenger, reply, NULL);
    ufo_message_free (reply);

    /* TODO: check that we don't need to execute this branch wen priv->input is null */
    if (priv->input_task && priv->input) {
        ufo_input_task_stop (UFO_INPUT_TASK (priv->input_task));

        ufo_input_task_release_input_buffer (UFO_INPUT_TASK (priv->input_task),
                                             priv->input);

        g_usleep (1.5 * G_USEC_PER_SEC);
        unref_and_free ((GObject **) &priv->input_task);
        unref_and_free ((GObject **) &priv->input);
    }

    unref_and_free ((GObject **) &priv->output_task);
    unref_and_free ((GObject **) &priv->task_graph);
}

static void
handle_terminate (UfoDaemon *daemon, UfoMessage *request)
{
    UfoDaemonPrivate *priv = UFO_DAEMON_GET_PRIVATE (daemon);
    UfoMessage *reply = ufo_message_new (UFO_MESSAGE_ACK, 0);

    ufo_messenger_send_blocking (priv->messenger, reply, NULL);
    ufo_message_free (reply);

    if (priv->scheduler_thread != NULL) {
        g_message ("Waiting for scheduler to finish ...");
        g_thread_join (priv->scheduler_thread);
        g_message ("Done.");
    }

    ufo_messenger_disconnect (priv->messenger);
}

static gpointer
run_scheduler (UfoDaemon *daemon)
{
    UfoDaemonPrivate *priv = UFO_DAEMON_GET_PRIVATE (daemon);

    g_message ("Run scheduler ...");
    priv->scheduler = ufo_scheduler_new ();
    ufo_base_scheduler_set_resources (priv->scheduler, priv->resources);
    ufo_base_scheduler_run (priv->scheduler, priv->task_graph, NULL);
    g_message ("Done.");

    g_object_unref (priv->scheduler);
    priv->scheduler = ufo_scheduler_new ();

    return NULL;
}

static void
ufo_daemon_start_impl (UfoDaemon *daemon)
{
    UfoDaemonPrivate *priv;
    GError *error = NULL;

    priv = UFO_DAEMON_GET_PRIVATE (daemon);
    priv->resources = ufo_resources_new (&error);

    if (error != NULL) {
        g_warning ("%s\n", error->message);
        return;
    }

    /* tell the calling thread that we have started */
    g_mutex_lock (priv->started_lock);
    priv->has_started = TRUE;
    g_cond_signal (priv->started_cond);
    g_mutex_unlock (priv->started_lock);

    gboolean wait_for_messages = TRUE;

    while (wait_for_messages) {
        GError *err = NULL;
        UfoMessage *message = ufo_messenger_recv_blocking (priv->messenger, &err);

        if (err != NULL) {
            /* If daemon is stopped, socket will be closed and message_recv will
             * yield an error - we stop. */
            wait_for_messages = FALSE;
        }
        else {
            if (message->type >= UFO_MESSAGE_INVALID_REQUEST)
                g_error ("Invalid request");
            else
                handlers[message->type](daemon, message);

            if (message->type == UFO_MESSAGE_TERMINATE)
                wait_for_messages = FALSE;
        }

        ufo_message_free (message);
    }

    /* tell calling thread we have stopped */
    g_mutex_lock (priv->stopped_lock);
    priv->has_stopped = TRUE;
    g_cond_signal (priv->stopped_cond);
    g_mutex_unlock (priv->stopped_lock);
}

void
ufo_daemon_start (UfoDaemon *daemon, GError **error)
{
    GError *tmp_error = NULL;
    UfoDaemonPrivate *priv = UFO_DAEMON_GET_PRIVATE (daemon);

    g_mutex_lock (priv->startstop_lock);

    if (priv->has_started) {
        g_mutex_unlock (priv->startstop_lock);
        return;
    }

    ufo_messenger_connect (priv->messenger, priv->listen_address, UFO_MESSENGER_SERVER, &tmp_error);

    if (tmp_error != NULL) {
        g_propagate_error (error, tmp_error);
        goto daemon_start_unlock;
    }

    priv->thread = g_thread_create ((GThreadFunc) ufo_daemon_start_impl, daemon, TRUE, NULL);
    g_return_if_fail (priv->thread != NULL);

    g_mutex_lock (priv->started_lock);

        while (!priv->has_started)
            g_cond_wait (priv->started_cond, priv->started_lock);

    g_mutex_unlock (priv->started_lock);

daemon_start_unlock:
    g_mutex_unlock (priv->startstop_lock);
}

void
ufo_daemon_stop (UfoDaemon *daemon, GError **error)
{
    GError *tmp_error = NULL;
    UfoDaemonPrivate *priv = UFO_DAEMON_GET_PRIVATE (daemon);

    g_mutex_lock (priv->startstop_lock);

    /* HACK we can't call _disconnect() as this has to be run from the
     * thread running the daemon which might be blocking on recv
     * - we thus send a TERMINATE message to that thread
     */

    UfoMessenger *tmp_messenger;
#ifdef WITH_MPI
    tmp_messenger = UFO_MESSENGER (ufo_mpi_messenger_new ());
#elif WITH_ZMQ
    tmp_messenger = UFO_MESSENGER (ufo_zmq_messenger_new ());
#endif

    ufo_messenger_connect (tmp_messenger, priv->listen_address, UFO_MESSENGER_CLIENT, &tmp_error);

    if (tmp_error != NULL) {
        g_propagate_error (error, tmp_error);
        goto daemon_stop_unlock;
    }

    UfoMessage *request = ufo_message_new (UFO_MESSAGE_TERMINATE, 0);
    ufo_messenger_send_blocking (tmp_messenger, request, NULL);

    g_thread_join (priv->thread);

    g_mutex_lock (priv->stopped_lock);

        priv->has_stopped = TRUE;
        g_cond_signal (priv->stopped_cond);

    g_mutex_unlock (priv->stopped_lock);

daemon_stop_unlock:
    g_mutex_unlock (priv->startstop_lock);
}

void ufo_daemon_wait_finish (UfoDaemon *daemon)
{
    UfoDaemonPrivate *priv = UFO_DAEMON_GET_PRIVATE (daemon);

    g_mutex_lock (priv->stopped_lock);
    while (!priv->has_stopped)
        g_cond_wait (priv->stopped_cond, priv->stopped_lock);
    g_mutex_unlock (priv->stopped_lock);
}

static void
ufo_daemon_dispose (GObject *object)
{
    UfoDaemonPrivate *priv;
    priv = UFO_DAEMON_GET_PRIVATE (object);

    if (priv->task_graph)
        g_object_unref (priv->task_graph);

    if (priv->messenger != NULL)
        g_object_unref (priv->messenger);

    if (priv->manager != NULL)
        g_object_unref (priv->manager);

    if (priv->scheduler != NULL)
        g_object_unref (priv->scheduler);

    G_OBJECT_CLASS (ufo_daemon_parent_class)->dispose (object);
}

static void
ufo_daemon_finalize (GObject *object)
{
    UfoDaemonPrivate *priv = UFO_DAEMON_GET_PRIVATE (object);
    g_mutex_free (priv->startstop_lock);
    g_cond_free (priv->started_cond);
    g_free (priv->listen_address);

    G_OBJECT_CLASS (ufo_daemon_parent_class)->finalize (object);
}

static void
ufo_daemon_class_init (UfoDaemonClass *klass)
{
    GObjectClass *oclass;

    oclass = G_OBJECT_CLASS (klass);
    oclass->dispose = ufo_daemon_dispose;
    oclass->finalize = ufo_daemon_finalize;

    g_type_class_add_private (klass, sizeof (UfoDaemonPrivate));
}

static void
ufo_daemon_init (UfoDaemon *self)
{
    UfoDaemonPrivate *priv;
    self->priv = priv = UFO_DAEMON_GET_PRIVATE (self);

    priv->scheduler = NULL;
    priv->startstop_lock = g_mutex_new ();
    priv->started_lock = g_mutex_new ();
    priv->stopped_lock = g_mutex_new ();
    priv->started_cond = g_cond_new ();
    priv->stopped_cond = g_cond_new ();
    priv->has_started = FALSE;
    priv->has_stopped = FALSE;
}
