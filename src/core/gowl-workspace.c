/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "gowl-workspace.h"

typedef struct {
	guint64  id;
	gchar   *name;
	guint32  tag_mask;
} GowlWorkspacePrivate;

G_DEFINE_TYPE_WITH_PRIVATE(GowlWorkspace, gowl_workspace, G_TYPE_OBJECT)

#define GET_PRIV(self) \
	((GowlWorkspacePrivate *) \
	 gowl_workspace_get_instance_private(self))

enum {
	PROP_0,
	PROP_ID,
	PROP_NAME,
	PROP_TAG_MASK,
	N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL };

static void
gowl_workspace_get_property(GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
	GowlWorkspace        *self = GOWL_WORKSPACE(object);
	GowlWorkspacePrivate *priv = GET_PRIV(self);

	switch (prop_id) {
	case PROP_ID:
		g_value_set_uint64(value, priv->id);
		break;
	case PROP_NAME:
		g_value_set_string(value, priv->name);
		break;
	case PROP_TAG_MASK:
		g_value_set_uint(value, priv->tag_mask);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gowl_workspace_set_property(GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
	GowlWorkspace        *self = GOWL_WORKSPACE(object);
	GowlWorkspacePrivate *priv = GET_PRIV(self);

	switch (prop_id) {
	case PROP_ID:
		priv->id = g_value_get_uint64(value);
		break;
	case PROP_NAME:
		gowl_workspace_set_name(self, g_value_get_string(value));
		break;
	case PROP_TAG_MASK:
		gowl_workspace_set_tag_mask(self, g_value_get_uint(value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gowl_workspace_finalize(GObject *object)
{
	GowlWorkspace        *self = GOWL_WORKSPACE(object);
	GowlWorkspacePrivate *priv = GET_PRIV(self);

	g_clear_pointer(&priv->name, g_free);

	G_OBJECT_CLASS(gowl_workspace_parent_class)->finalize(object);
}

static void
gowl_workspace_class_init(GowlWorkspaceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->get_property = gowl_workspace_get_property;
	object_class->set_property = gowl_workspace_set_property;
	object_class->finalize     = gowl_workspace_finalize;

	props[PROP_ID] = g_param_spec_uint64(
		"id", "Id",
		"Monotonic numeric identifier",
		0, G_MAXUINT64, GOWL_WORKSPACE_ID_INVALID,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
		| G_PARAM_STATIC_STRINGS);

	props[PROP_NAME] = g_param_spec_string(
		"name", "Name",
		"Optional display name",
		NULL,
		G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY
		| G_PARAM_STATIC_STRINGS);

	props[PROP_TAG_MASK] = g_param_spec_uint(
		"tag-mask", "Tag Mask",
		"Bitmask of active tags inside this workspace",
		0, G_MAXUINT32, 0,
		G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY
		| G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, N_PROPS, props);
}

static void
gowl_workspace_init(GowlWorkspace *self)
{
	GowlWorkspacePrivate *priv = GET_PRIV(self);

	priv->id       = GOWL_WORKSPACE_ID_INVALID;
	priv->name     = NULL;
	priv->tag_mask = 0;
}

GowlWorkspace *
gowl_workspace_new(guint64      id,
                    const gchar *name,
                    guint32      tag_mask)
{
	return g_object_new(GOWL_TYPE_WORKSPACE,
	                    "id",       id,
	                    "name",     name,
	                    "tag-mask", tag_mask,
	                    NULL);
}

guint64
gowl_workspace_get_id(GowlWorkspace *self)
{
	g_return_val_if_fail(GOWL_IS_WORKSPACE(self),
	                     GOWL_WORKSPACE_ID_INVALID);
	return GET_PRIV(self)->id;
}

GowlWorkspaceId *
gowl_workspace_get_id_boxed(GowlWorkspace *self)
{
	g_return_val_if_fail(GOWL_IS_WORKSPACE(self), NULL);
	return gowl_workspace_id_new(GET_PRIV(self)->id);
}

const gchar *
gowl_workspace_get_name(GowlWorkspace *self)
{
	g_return_val_if_fail(GOWL_IS_WORKSPACE(self), NULL);
	return GET_PRIV(self)->name;
}

void
gowl_workspace_set_name(GowlWorkspace *self, const gchar *name)
{
	GowlWorkspacePrivate *priv;

	g_return_if_fail(GOWL_IS_WORKSPACE(self));
	priv = GET_PRIV(self);

	if (g_strcmp0(priv->name, name) == 0)
		return;

	g_free(priv->name);
	priv->name = g_strdup(name);
	g_object_notify_by_pspec(G_OBJECT(self), props[PROP_NAME]);
}

guint32
gowl_workspace_get_tag_mask(GowlWorkspace *self)
{
	g_return_val_if_fail(GOWL_IS_WORKSPACE(self), 0);
	return GET_PRIV(self)->tag_mask;
}

void
gowl_workspace_set_tag_mask(GowlWorkspace *self, guint32 mask)
{
	GowlWorkspacePrivate *priv;

	g_return_if_fail(GOWL_IS_WORKSPACE(self));
	priv = GET_PRIV(self);

	if (priv->tag_mask == mask)
		return;

	priv->tag_mask = mask;
	g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TAG_MASK]);
}
