/* Compile: gcc -Wall -O2 -g -o magnet magnet.c -lfcgi -llua -lm -ldl -pedantic -ansi -std=c99 */

#include <sys/stat.h>    /* stat()                            */
#include <assert.h>      /* assert() -- *duh*                 */
#include <stdio.h>       /* fwrite(), fprintf(), fputs(), ... */
#include <stdlib.h>      /* EXIT_SUCCESS, EXIT_FAILURE        */
#include <fcgi_stdio.h>  /* FCGI_Accept()                     */
#include <lualib.h>      /* LUA'Y STUFF :D-S-<                */
#include <lauxlib.h>

static int
magnet_print(lua_State * const L)
{
	const size_t nargs = lua_gettop(L);
	if (nargs)
	{
		char *s;
		size_t i, s_len;

		lua_getglobal(L, "tostring");
		assert(lua_isfunction(L, -1));

		for (i = 1; i <= nargs; i++)
		{
			lua_pushvalue(L, -1);                      /* Push tostring()                      */
			lua_pushvalue(L,  i);                      /* Push argument                        */
			lua_call(L, 1, 1);                         /* tostring(argument), take 1, return 1 */
			s = (char *) lua_tolstring(L, -1, &s_len); /* Fetch result.                        */
			
			if (s == NULL)
				return luaL_error(L, LUA_QL("tostring") " must return a string to " LUA_QL("print"));

			fwrite(s, 1, s_len, stdout);

			/* Pop <tostring(argument)> */
			lua_pop(L, 1);
		}
	}
	/* Nothing returned on the Lua stack,
	** so return 0; (exposed cfunction ) */
	return 0;
}

static int
magnet_cache_script(lua_State * const L, const char * const fn, const time_t mtime)
{
	/* Compile it as a chunk, push it as a function onto the Lua stack. */
	if (luaL_loadfile(L, fn))
	{
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		lua_pop(L, 1); /* remove the error-msg */
		return EXIT_FAILURE;
	}

	/* Push _G.magnet and _G.magnet.cache onto the Lua stack. */
	lua_getfield(L, LUA_GLOBALSINDEX, "magnet");
	lua_getfield(L, -1, "cache");

	/* <table>.script = <func> */
	lua_newtable(L); 
	assert(lua_isfunction(L, -4));
	lua_pushvalue(L, -4); 
	lua_setfield(L, -2, "script"); /* Pops the function. */

	/* <table>.mtime = <mtime> */
	lua_pushinteger(L, mtime);
	lua_setfield(L, -2, "mtime");  /* Pops the mtime integer. */

	/* <table>.hits = <hits> */
	lua_pushinteger(L, 0);
	lua_setfield(L, -2, "hits");   /* Pops the hits integer. */

	/* magnet.cache['<script>'] = <table> */
	lua_setfield(L, -2, fn);       /* Pops the "anonymous" table. */

	lua_pop(L, 2); /* Pop _G.magnet and _G.magnet.cache */

	/* On the stack should be the function itself. */
	assert(lua_isfunction(L, lua_gettop(L)));

	return 0;
}

static int
magnet_get_script(lua_State * const L, const char * const fn)
{
	struct stat st;
	time_t mtime = 0;

	assert(lua_gettop(L) == 0);

	if (stat(fn, &st) == -1)
		return EXIT_FAILURE;

	mtime = st.st_mtime;

	lua_getfield(L, LUA_GLOBALSINDEX, "magnet"); 
	assert(lua_istable(L, -1));
	lua_getfield(L, -1, "cache");
	assert(lua_istable(L, -1));

	lua_getfield(L, -1, fn);

	/* magnet.cache['<script>'] is not a table for some reason, re-cache. */
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 3); /* Pop the nil. */
		if (magnet_cache_script(L, fn, mtime))
			return EXIT_FAILURE;
	}
	else
	{
		lua_getfield(L, -1, "mtime");
		assert(lua_isnumber(L, -1));

		/* Script has not been modified, continue as usual. */
		if (mtime == lua_tointeger(L, -1))
		{
			lua_Integer hits;
			lua_pop(L, 1);
	
			/* Increment the hit counter. */	
			lua_getfield(L, -1, "hits");
			hits = lua_tointeger(L, -1);
			lua_pop(L, 1);
			lua_pushinteger(L, hits + 1);
			lua_setfield(L, -2, "hits");

			/* It is in the cache. */
			assert(lua_istable(L, -1));
			lua_getfield(L, -1, "script");
			assert(lua_isfunction(L, -1));
			lua_insert(L, -4);
			lua_pop(L, 3);
			assert(lua_isfunction(L, -1));
		}
		/* Recorded magnet.cache['<script>'].mtime does
		** not match the actual mtime, re-cache. */
		else
		{
			lua_pop(L, 4);
			if (magnet_cache_script(L, fn, mtime))
				return EXIT_FAILURE;
		}
	}
	/* This should be the function (top of Lua stack). */
	assert(lua_gettop(L) == 1);

	return EXIT_SUCCESS;
}

int
main(void)
{
	lua_State * const L = luaL_newstate(); 

	luaL_openlibs(L);

	lua_newtable(L); /* magnet. */
	lua_newtable(L); /* magnet.cache. */
	lua_setfield(L, -2, "cache");
	lua_setfield(L, LUA_GLOBALSINDEX, "magnet");

	while (FCGI_Accept() >= 0)
	{
		assert(lua_gettop(L) == 0);

		if (magnet_get_script(L, getenv("SCRIPT_FILENAME")))
		{
			fputs("Status: 404\r\n\r\n", stdout);
			assert(lua_gettop(L) == 0);
			continue;
		}
		/**
		 * We want to create empty environment for our script. 
		 * 
		 * setmetatable({}, {__index = _G})
		 * 
		 * If a function symbol is not defined in our env,
		 * __index will look it up in the global env. 
		 *
		 * All variables created in the script-env will be thrown 
		 * away at the end of the script run. */

		/* Empty environment; will become parent to _G._G */
		lua_newtable(L);

		/* We have to overwrite the print function */
		lua_pushcfunction(L, magnet_print);
		lua_setfield(L, -2, "print");

		lua_newtable(L);                    /* The meta-table for the new env.          */
		lua_pushvalue(L, LUA_GLOBALSINDEX);

		lua_setfield(L,     -2, "__index"); /* { __index = _G }                         */
		lua_setmetatable(L, -2);            /* setmetatable({}, { __index = _G })       */
		lua_setfenv(L,      -2);            /* On the stack should be the modified env. */

		/* The top of the stack is the function from magnet_get_script() again. */
		if (lua_pcall(L, 0, 1, 0))
		{
			/* Unsure about this. */
			/* fprintf(stderr, "%s\n", lua_tostring(L, -1)); */
			fputs("Status: 503\r\n\r\n", stdout);
			fputs(lua_tostring(L, -1)  , stdout);

			/* Remove the error message. */
			lua_pop(L, 1);

			continue;
		}

		/* Remove the function copy from the stack. */
		lua_pop(L, 1);
		assert(lua_gettop(L) == 0);
	}

	lua_close(L);
	return EXIT_SUCCESS;
}	
