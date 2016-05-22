

/*
 * @copyright
 *
 *  Copyright 2016 Neo Natura
 *
 *  This file is part of the Share Library.
 *  (https://github.com/neonatura/share)
 *        
 *  The Share Library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version. 
 *
 *  The Share Library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with The Share Library.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  @endcopyright
 */  


#ifndef __BLOCKCHAIN__BC_BLOCK_H__
#define __BLOCKCHAIN__BC_BLOCK_H__



int bc_open(char *name, bc_t **bc_p);

char *bc_name(bc_t *bc);

int bc_read(bc_t *bc, int pos, void *data, size_t data_len);

int bc_append(bc_t *bc, bc_hash_t hash, void *data, size_t data_len);

int bc_find(bc_t *bc, bc_hash_t hash, int *pos_p);

int bc_get(bc_t *bc, int pos, unsigned char **data_p, size_t *data_len_p);

int bc_alloc(bc_t *bc, unsigned int jrnl);

int bc_get_hash(bc_t *bc, int pos, bc_hash_t ret_hash);

int bc_hash_cmp(bc_hash_t a_hash, bc_hash_t b_hash);

int bc_idx_new(bc_t *bc, int pos, bc_hash_t hash, size_t data_len);

int bc_write(bc_t *bc, int pos, bc_hash_t hash, void *raw_data, int data_len);

void bc_close(bc_t *bc);

int bc_purge(bc_t *bc, int pos);


#endif /* ndef __BLOCKCHAIN__BC_BLOCK_H__ */