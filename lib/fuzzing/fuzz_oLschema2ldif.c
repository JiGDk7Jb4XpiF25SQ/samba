/*
   Fuzzing for oLschema2ldif
   Copyright (C) Michael Hanselmann 2019

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "fuzzing.h"
#include "utils/oLschema2ldif/lib.h"

static FILE *devnull;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	devnull = fopen("/dev/null", "w");

	return 0;
}

int LLVMFuzzerTestOneInput(uint8_t *buf, size_t len)
{
	TALLOC_CTX *mem_ctx;
	struct conv_options opt;

	mem_ctx = talloc_init(__FUNCTION__);

	opt.in = fmemopen(buf, len, "r");
	opt.out = devnull;
	opt.ldb_ctx = ldb_init(mem_ctx, NULL);

	opt.basedn = ldb_dn_new(mem_ctx, opt.ldb_ctx, "");

	process_file(mem_ctx, &opt);

	fclose(opt.in);

	talloc_free(mem_ctx);

	return 0;
}
