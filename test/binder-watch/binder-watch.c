/*
 * Copyright (C) 2026 Jolla Mobile Ltd
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gbinder.h>

#include <glib-unix.h>

#include <gutil_log.h>

#define DEFAULT_DEVICE  "/dev/binder"

#define RET_OK          (0)
#define RET_ERR         (1)
#define RET_INVARG      (2)

typedef struct app_options {
    char* dev;
    const char* fqname;
    const char* suffix;
    gboolean exit_when_found;
} AppOptions;

typedef struct app {
    const AppOptions* opt;
    GMainLoop* loop;
    GBinderServiceManager* sm;
    GBinderRemoteObject* remote;
    gulong wait_id;
    gulong death_id;
    int ret;
} App;

static
gboolean
app_signal(
    gpointer user_data)
{
    App* app = user_data;

    GINFO("Caught signal, shutting down...");
    g_main_loop_quit(app->loop);
    return G_SOURCE_CONTINUE;
}

static
void
app_remote_drop(
    App* app)
{
    gbinder_remote_object_remove_handler(app->remote, app->death_id);
    gbinder_remote_object_unref(app->remote);
    app->death_id = 0;
    app->remote = NULL;
}

static
void
app_remote_gone(
    GBinderRemoteObject* obj,
    void* user_data)
{
    App* app = user_data;

    GINFO("Service %s is gone", app->opt->fqname);
    app_remote_drop(app);
}

static
gboolean
app_remote_attach(
    App* app)
{
    app->remote = gbinder_servicemanager_get_service_sync(app->sm,
        app->opt->fqname, NULL);
    if (app->remote) {
        gbinder_remote_object_ref(app->remote);
        app->death_id = gbinder_remote_object_add_death_handler(app->remote,
            app_remote_gone, app);
        return TRUE;
    } else {
        return FALSE;
    }
}

static
void
app_registration_handler(
    GBinderServiceManager* sm,
    const char* name,
    void* user_data)
{
    App* app = user_data;
    const AppOptions* opt = app->opt;

    if (!strcmp(name, opt->fqname)) {
        GINFO("%s appeared", name);
        if (opt->exit_when_found) {
            g_main_loop_quit(app->loop);
            app->ret = RET_OK;
        } else {
            app_remote_attach(app);
        }
    } else {
        GDEBUG("Ignoring %s", name);
    }
}

static
void
app_run(
   App* app)
{
    const AppOptions* opt = app->opt;

    app->sm = gbinder_servicemanager_new(opt->dev);
    if (app_remote_attach(app)) {
        GINFO("Service %s is there", opt->fqname);
    }

    if (!app->remote || !opt->exit_when_found) {
        guint sigtrm = g_unix_signal_add(SIGTERM, app_signal, app);
        guint sigint = g_unix_signal_add(SIGINT, app_signal, app);

        GINFO("Watching for %s", opt->fqname);
        app->wait_id = gbinder_servicemanager_add_registration_handler(app->sm,
            opt->fqname, app_registration_handler, app);

        app->loop = g_main_loop_new(NULL, TRUE);
        app->ret = RET_OK;
        g_main_loop_run(app->loop);

        g_source_remove(sigtrm);
        g_source_remove(sigint);
        g_main_loop_unref(app->loop);

        gbinder_servicemanager_remove_handler(app->sm, app->wait_id);
    }

    app_remote_drop(app);
    gbinder_servicemanager_unref(app->sm);
}

static
gboolean
app_log_verbose(
    const gchar* name,
    const gchar* value,
    gpointer data,
    GError** error)
{
    gutil_log_default.level = GLOG_LEVEL_VERBOSE;
    return TRUE;
}

static
gboolean
app_log_quiet(
    const gchar* name,
    const gchar* value,
    gpointer data,
    GError** error)
{
    gutil_log_default.level = GLOG_LEVEL_ERR;
    return TRUE;
}

static
gboolean
app_init(
    AppOptions* opt,
    int argc,
    char* argv[])
{
    gboolean ok = FALSE;
    GOptionEntry entries[] = {
        { "device", 'd', 0, G_OPTION_ARG_STRING, &opt->dev,
          "Binder device [" DEFAULT_DEVICE "]", "DEVICE" },
        { "wait", 'w', 0, G_OPTION_ARG_NONE, &opt->exit_when_found,
          "Wait until the service is registered and exit", NULL},
        { "verbose", 'v', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
          app_log_verbose, "Enable verbose output", NULL },
        { "quiet", 'q', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
          app_log_quiet, "Be quiet", NULL },
        { NULL }
    };

    GError* error = NULL;
    GOptionContext* options = g_option_context_new("FQNAME");

    g_option_context_set_summary(options, "Tool for watching binder service "
    "names in servicemanager.");
    g_option_context_add_main_entries(options, entries, NULL);

    memset(opt, 0, sizeof(*opt));
    if (g_option_context_parse(options, &argc, &argv, &error) &&
        argc == 2 && argv[1][0]) {
        if (!opt->dev || !opt->dev[0]) {
            opt->dev = g_strdup(DEFAULT_DEVICE);
        }
        opt->fqname = argv[1];
        ok = TRUE;
    } else if (error) {
        GERR("%s", error->message);
        g_error_free(error);
    } else {
        char* help = g_option_context_get_help(options, TRUE, NULL);

        fprintf(stderr, "%s", help);
        g_free(help);
        ok = FALSE;
    }
    g_option_context_free(options);
    return ok;
}

int main(int argc, char* argv[])
{
    App app;
    AppOptions opt;

    memset(&app, 0, sizeof(app));
    app.ret = RET_INVARG;
    app.opt = &opt;
    if (app_init(&opt, argc, argv)) {
        app_run(&app);
    }
    g_free(opt.dev);
    return app.ret;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
