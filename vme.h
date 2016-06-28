#ifndef UUID_ac4de271_77a8_495e_b37b_999535689192
#define UUID_ac4de271_77a8_495e_b37b_999535689192
/* VME constants

 * Copyright (C) 2016  Florian Lüke <f.lueke@mesytec.com>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */


/* PRIV means privileged (supervisory access)
 * USER means non-privileged access
 */

// single word transfer
#define VME_AM_A32_PRIV_DATA  0x0d
#define VME_AM_A32_USER_DATA  0x09
#define VME_AM_A32_PRIV_PROG  0x0e
#define VME_AM_A32_USER_PROG  0x0a

// block transfer
#define VME_AM_A32_PRIV_BLT   0x0f
#define VME_AM_A32_USER_BLT   0x0b

// multiplexed block transfer
#define VME_AM_A32_PRIV_MBLT  0x0c
#define VME_AM_A32_USER_MBLT  0x08

#endif
