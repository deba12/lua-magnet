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
		const char *s;
		size_t i, s_len;

		lua_getglobal(L, "tostring");
		assert(lua_isfunction(L, -1));

		/* We call the Lua tostring() so it may invoke __tostring */
		for (i = 1; i <= nargs; i++)
		{
			lua_pushvalue    (L, -1        );          /* Push tostring()                                             */
			lua_pushvalue    (L,  i        );          /* Push <argument>                                             */
			lua_call         (L,  1,      1);          /* Pushes tostring(<argument>), pops tostring() and <argument> */
			s = lua_tolstring(L, -1, &s_len);          /* const char *s = <string>                                    */
			if (s == NULL)                             /* Something went wrong returning a pointer to <string>, error */
				return luaL_error(L, LUA_QL("tostring") " must return a string to " LUA_QL("print"));
			fwrite((char *) s, 1, s_len, stdout);      /* Write the string we worked so hard for. >.<                 */
			lua_pop(L, 1);                             /* Pop <string>                                                */
		}
	}
	/* Returning nothing on the
	** Lua stack, so return 0 */
	return 0;
}

static int
magnet_cache_script(lua_State * const L, const char * const fn, const time_t mtime)
{
	/* Convert `fn` to a function -> onto the Lua stack
	** Errors with non-0 (reason for the unconventional if) */
	if (luaL_loadfile(L, fn))
	{
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		lua_pop(L, 1);       Pop error message
		return EXIT_FAILURE;
	}

	lua_getfield   (L, LUA_GLOBALSINDEX,     "magnet"); /* Push _G.magnet                                                  */
	lua_getfield   (L,               -1,      "cache"); /* Push _G.magnet.cache                                            */
	lua_newtable   (L                                ); /* Push <table>                                                    */
	lua_pushvalue  (L,               -4              ); /* Push loadfile() <function>                                      */
	lua_setfield   (L,               -2, "scriptfunc"); /* <table>.scriptfunc = <function>, pop <function>                 */
	lua_pushinteger(L,            mtime              ); /* Push `mtime`                                                    */
	lua_setfield   (L,               -2,      "mtime"); /* <table>.mtime = <mtime>, pop <mtime>                            */
	lua_pushinteger(L,                0              ); /* Push 0                                                          */
	lua_setfield   (L,               -2,       "hits"); /* <table>.hits = 0, pops 0                                        */
	lua_setfield   (L,               -2,           fn); /* _G.magnet.cache['`fn`'] = <table>, pops <table>                 */
	lua_pop        (L,                2              ); /* Pop _G.magnet and _G.magnet.cache                               */

	/* Only thing on the stack should be the script-function itself (from loadfile()) */
	assert(lua_isfunction(L, lua_gettop(L)));
	return EXIT_SUCCESS;
}

static int
magnet_get_script(lua_State * const L, const char *fn)
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
	lua_State *L = luaL_newstate(); 

	L = luaL_newstate();
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
