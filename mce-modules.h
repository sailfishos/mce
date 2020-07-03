/**
 * @file mce-modules.h
 * Headers for the module handling for MCE
 * <p>
 * Copyright © 2007 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2018-2019 Jolla Ltd.
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _MCE_MODULES_H_
#define _MCE_MODULES_H_

#include <glib.h>

/** Name of Modules configuration group */
#define MCE_CONF_MODULES_GROUP          "Modules"

/** Name of configuration key for module path */
#define MCE_CONF_MODULES_PATH           "ModulePath"

/** Name of configuration key for modules to load */
#define MCE_CONF_MODULES_MODULES        "Modules"

/** Default value for module path */
#define DEFAULT_MCE_MODULE_PATH         G_STRINGIFY(MCE_DEFAULT_MCE_MODULE_PATH)

void mce_modules_dump_info(void);
gboolean mce_modules_init(void);
void mce_modules_exit(void);

#endif /* _MCE_MODULES_H_ */
