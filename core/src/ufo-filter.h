#ifndef __UFO_FILTER_H
#define __UFO_FILTER_H

#include <glib.h>
#include <glib-object.h>
#include <ethos/ethos.h>

#include "ufo-buffer.h"

#define UFO_TYPE_FILTER             (ufo_filter_get_type())
#define UFO_FILTER(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), UFO_TYPE_FILTER, UfoFilter))
#define UFO_IS_FILTER(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), UFO_TYPE_FILTER))
#define UFO_FILTER_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), UFO_TYPE_FILTER, UfoFilterClass))
#define UFO_IS_FILTER_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), UFO_TYPE_FILTER))
#define UFO_FILTER_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS((obj), UFO_TYPE_FILTER, UfoFilterClass))

typedef struct _UfoFilter           UfoFilter;
typedef struct _UfoFilterClass      UfoFilterClass;
typedef struct _UfoFilterPrivate    UfoFilterPrivate;

struct _UfoFilter {
    EthosPlugin parent;

    /* public */
    gboolean is_gpu_enabled;

    /* private */
    UfoFilterPrivate *priv;
};

struct _UfoFilterClass {
    EthosPluginClass parent_class;

    /* class members */

    /* virtual public methods */
    void (*process) (UfoFilter *self, UfoBuffer *input, UfoBuffer *output);
};

/* virtual public methods */
void ufo_filter_process(UfoFilter *self, UfoBuffer *input, UfoBuffer *output);

/* non-virtual public methods */

GType ufo_filter_get_type(void);

#endif
