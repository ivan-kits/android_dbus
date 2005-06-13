/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-gtype-specialized.h: Non-DBus-specific functions for specialized GTypes
 *
 * Copyright (C) 2005 Red Hat, Inc.
 *
 * Licensed under the Academic Free License version 2.1
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifndef DBUS_GOBJECT_TYPE_SPECIALIZED_H
#define DBUS_GOBJECT_TYPE_SPECIALIZED_H

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

GType          dbus_g_type_get_collection                   (const char *container,
							     GType       specialization);
GType          dbus_g_type_get_map                          (const char *container,
							     GType       key_specialization,
							     GType       value_specialization);
gboolean       dbus_g_type_is_collection                    (GType       gtype);
gboolean       dbus_g_type_is_map                           (GType       gtype);
GType          dbus_g_type_get_collection_specialization    (GType       gtype);
GType          dbus_g_type_get_map_key_specialization       (GType       gtype);
GType          dbus_g_type_get_map_value_specialization     (GType       gtype);

typedef void   (*DBusGTypeSpecializedCollectionIterator)    (const GValue *val,
							     gpointer      user_data);
typedef void   (*DBusGTypeSpecializedMapIterator)           (const GValue *key_val,
							     const GValue *value_val,
							     gpointer      user_data);

gpointer       dbus_g_type_specialized_construct            (GType type);

gboolean       dbus_g_type_collection_get_fixed             (GValue                                 *value,
							     gpointer                               *data,
							     guint                                  *len);

void           dbus_g_type_collection_value_iterate         (GValue                                 *value,
							     DBusGTypeSpecializedCollectionIterator  iterator,
							     gpointer                                user_data);

void           dbus_g_type_map_value_iterate                (GValue                                 *value,
							     DBusGTypeSpecializedMapIterator         iterator,
							     gpointer                                user_data);

typedef gpointer (*DBusGTypeSpecializedConstructor) (GType type);
typedef void     (*DBusGTypeSpecializedFreeFunc)    (GType type, gpointer val);
typedef gpointer (*DBusGTypeSpecializedCopyFunc)    (GType type, gpointer src);

typedef struct {
  DBusGTypeSpecializedConstructor    constructor;
  DBusGTypeSpecializedFreeFunc       free_func;
  DBusGTypeSpecializedCopyFunc       copy_func;
  gpointer                           padding1;
  gpointer                           padding2;
  gpointer                           padding3;
} DBusGTypeSpecializedVtable;

typedef gboolean (*DBusGTypeSpecializedCollectionFixedAccessorFunc) (GType type, gpointer instance, gpointer *values, guint *len);
typedef void (*DBusGTypeSpecializedCollectionIteratorFunc) (GType type, gpointer instance, DBusGTypeSpecializedCollectionIterator iterator, gpointer user_data);

typedef struct {
  DBusGTypeSpecializedVtable                        base_vtable;
  DBusGTypeSpecializedCollectionFixedAccessorFunc   fixed_accessor;
  DBusGTypeSpecializedCollectionIteratorFunc        iterator;
} DBusGTypeSpecializedCollectionVtable;

typedef void (*DBusGTypeSpecializedMapIteratorFunc) (GType type, gpointer instance, DBusGTypeSpecializedMapIterator iterator, gpointer user_data);

typedef struct {
  DBusGTypeSpecializedVtable                        base_vtable;
  DBusGTypeSpecializedMapIteratorFunc               iterator;
} DBusGTypeSpecializedMapVtable;

void           dbus_g_type_specialized_init           (void);

void           dbus_g_type_register_collection        (const char                                   *name,
						       const DBusGTypeSpecializedCollectionVtable   *vtable,
						       guint                                         flags);
  
void           dbus_g_type_register_map               (const char                                   *name,
						       const DBusGTypeSpecializedMapVtable          *vtable,
						       guint                                         flags);

G_END_DECLS

#endif
