/** \file autoload.cpp
	
The classes responsible for autoloading functions and completions.
*/

#include "config.h"
#include "autoload.h"
#include "wutil.h"
#include "common.h"
#include "signal.h"
#include "env.h"
#include "builtin_scripts.h"
#include "exec.h"
#include <assert.h>
#include <algorithm>

/* The time before we'll recheck an autoloaded file */
static const int kAutoloadStalenessInterval = 1;

file_access_attempt_t access_file(const wcstring &path, int mode) {
    file_access_attempt_t result = {0};
    struct stat statbuf;
    if (wstat(path.c_str(), &statbuf)) {
        result.error = errno;
    } else {
        result.mod_time = statbuf.st_mtime;
        if (waccess(path.c_str(), mode)) {
            result.error = errno;
        } else {
            result.accessible = true;
        }
    }
    
    // Note that we record the last checked time after the call, on the assumption that in a slow filesystem, the lag comes before the kernel check, not after.
    result.stale = false;
    result.last_checked = time(NULL);
    return result;
}

lru_cache_impl_t::lru_cache_impl_t(size_t size) : max_node_count(size), node_count(0), mouth(L"") {
    /* Hook up the mouth to itself: a one node circularly linked list */
    mouth.prev = mouth.next = &mouth;
}

void lru_cache_impl_t::node_was_evicted(lru_node_t *node) { }

void lru_cache_impl_t::evict_node(lru_node_t *condemned_node) {
    /* We should never evict the mouth */
    assert(condemned_node != NULL && condemned_node != &mouth);

    /* Remove it from the linked list */
    condemned_node->prev->next = condemned_node->next;
    condemned_node->next->prev = condemned_node->prev;

    /* Remove us from the set */
    node_set.erase(condemned_node);
    node_count--;

    /* Tell ourselves */
    this->node_was_evicted(condemned_node);
}

void lru_cache_impl_t::evict_last_node(void) {
    /* Simple */
    evict_node(mouth.prev);
}

bool lru_cache_impl_t::evict_node(const wcstring &key) {
    /* Construct a fake node as our key */
    lru_node_t node_key(key);
    
    /* Look for it in the set */
    node_set_t::iterator iter = node_set.find(&node_key);
    if (iter == node_set.end())
        return false;
    
    /* Evict the given node */
    evict_node(*iter);
    return true;
}

void lru_cache_impl_t::promote_node(lru_node_t *node) {
    /* We should never promote the mouth */
    assert(node != &mouth);
    
    /* First unhook us */
    node->prev->next = node->next;
    node->next->prev = node->prev;
    
    /* Put us after the mouth */
    node->next = mouth.next;
    node->next->prev = node;
    node->prev = &mouth;
    mouth.next = node;
}

bool lru_cache_impl_t::add_node(lru_node_t *node) {
    /* Add our node without eviction */
    if (! this->add_node_without_eviction(node))
        return false;
    
    /* Evict */
    while (node_count > max_node_count)
        evict_last_node();
    
    /* Success */
    return true;
}

bool lru_cache_impl_t::add_node_without_eviction(lru_node_t *node) {
    assert(node != NULL && node != &mouth);
    
    /* Try inserting; return false if it was already in the set */
    if (! node_set.insert(node).second)
        return false;
    
    /* Add the node after the mouth */
    node->next = mouth.next;
    node->next->prev = node;
    node->prev = &mouth;
    mouth.next = node;
    
    /* Update the count */
    node_count++;
    
    /* Evict */
    while (node_count > max_node_count)
        evict_last_node();
    
    /* Success */
    return true;
}

lru_node_t *lru_cache_impl_t::get_node(const wcstring &key) {
    lru_node_t *result = NULL;
    
    /* Construct a fake node as our key */
    lru_node_t node_key(key);
    
    /* Look for it in the set */
    node_set_t::iterator iter = node_set.find(&node_key);
    
    /* If we found a node, promote and return it */
    if (iter != node_set.end()) {
        result = *iter;
        promote_node(result);
    }
    return result;
}

void lru_cache_impl_t::evict_all_nodes() {
    while (node_count > 0) {
        evict_last_node();
    }
}



autoload_t::autoload_t(const wcstring &env_var_name_var, const builtin_script_t * const scripts, size_t script_count) :
                       env_var_name(env_var_name_var),
                       builtin_scripts(scripts),
                       builtin_script_count(script_count)
{
    pthread_mutex_init(&lock, NULL);
}

autoload_t::~autoload_t() {
    pthread_mutex_destroy(&lock);
}

void autoload_t::node_was_evicted(autoload_function_t *node) {
    // This should only ever happen on the main thread
    ASSERT_IS_MAIN_THREAD();
    
    // Tell ourselves that the command was removed if it was loaded
    if (! node->is_loaded)
        this->command_removed(node->key);
    delete node;
}

int autoload_t::unload( const wcstring &cmd )
{
    return this->evict_node(cmd);
}

int autoload_t::load( const wcstring &cmd, bool reload )
{
	int res;
	
	CHECK_BLOCK( 0 );
    ASSERT_IS_MAIN_THREAD();
    
	env_var_t path_var;
    
    /* Do some work while locked, including determing the path variable */
    {
        scoped_lock locker(lock); 
        path_var = env_get_string( env_var_name.c_str() );
        
        /*
          Do we know where to look?
        */
        if( path_var.empty() )
            return 0;
        
        /*
          Check if the lookup path has changed. If so, drop all loaded
          files.
        */
        if( path_var != this->path )
        {
            this->path = path_var;
            this->evict_all_nodes();
        }
    }
    
    /** Warn and fail on infinite recursion. It's OK to do this because this function is only called on the main thread. */
    if (this->is_loading(cmd))
    {
        debug( 0, 
               _( L"Could not autoload item '%ls', it is already being autoloaded. " 
                  L"This is a circular dependency in the autoloading scripts, please remove it."), 
               cmd.c_str() );
        return 1;
    }
    
    /* Mark that we're loading this */
    is_loading_set.insert(cmd);

    
    /* Get the list of paths from which we will try to load */
    std::vector<wcstring> path_list;
	tokenize_variable_array2( path_var, path_list );

	/* Try loading it */
	res = this->locate_file_and_maybe_load_it( cmd, true, reload, path_list );
    
    /* Clean up */
    int erased = is_loading_set.erase(cmd);
    assert(erased);
		
	return res;
}

bool autoload_t::can_load( const wcstring &cmd, const env_vars &vars )
{
    const wchar_t *path_var_ptr = vars.get(env_var_name.c_str());
    if (! path_var_ptr || ! path_var_ptr[0])
        return false;
    
    const wcstring path_var(path_var_ptr);        
    std::vector<wcstring> path_list;
	tokenize_variable_array2( path_var, path_list );
    return this->locate_file_and_maybe_load_it( cmd, false, false, path_list );
}

static bool script_name_precedes_script_name(const builtin_script_t &script1, const builtin_script_t &script2)
{
    return wcscmp(script1.name, script2.name) < 0;
}

void autoload_t::unload_all(void) {
    scoped_lock locker(lock);
    this->evict_all_nodes();
}

/**
   This internal helper function does all the real work. By using two
   functions, the internal function can return on various places in
   the code, and the caller can take care of various cleanup work.
   
     cmd: the command name ('grep')
     really_load: whether to actually parse it as a function, or just check it it exists
     reload: whether to reload it if it's already loaded
     path_list: the set of paths to check
     
     Result: if really_load is true, returns whether the function was loaded. Otherwise returns whether the function existed.
*/
bool autoload_t::locate_file_and_maybe_load_it( const wcstring &cmd, bool really_load, bool reload, const wcstring_list_t &path_list )
{
    /* Note that we are NOT locked in this function! */
	size_t i;
	bool reloaded = 0;

    /* Try using a cached function. If we really want the function to be loaded, require that it be really loaded. If we're not reloading, allow stale functions. */
    {
        bool allow_stale_functions = ! reload;
        
        /* Take a lock */
        scoped_lock locker(lock);
        
        /* Get the function */
        autoload_function_t * func = this->get_node(cmd);
        
        /* Determine if we can use this cached function */
        bool use_cached;
        if (! func) {
            /* Can't use a function that doesn't exist */
            use_cached = false;
        } else if ( ! allow_stale_functions && time(NULL) - func->access.last_checked > kAutoloadStalenessInterval) {
            /* Can't use a stale function */
            use_cached = false;
        } else if (really_load && ! func->is_loaded) {
            /* Can't use an unloaded function */
            use_cached = false;
        } else {
            /* I guess we can use it */
            use_cached = true;
        }
        
        /* If we can use this function, return whether we were able to access it */
        if (use_cached) {
            return func->access.accessible;
        }
    }    
    /* The source of the script will end up here */
    wcstring script_source;
    bool has_script_source = false;
    
    /* Whether we found an accessible file */
    bool found_file = false;
    
    /* Look for built-in scripts via a binary search */
    const builtin_script_t *matching_builtin_script = NULL;
    if (builtin_script_count > 0)
    {
        const builtin_script_t test_script = {cmd.c_str(), NULL};
        const builtin_script_t *array_end = builtin_scripts + builtin_script_count;
        const builtin_script_t *found = std::lower_bound(builtin_scripts, array_end, test_script, script_name_precedes_script_name);
        if (found != array_end && ! wcscmp(found->name, test_script.name))
        {
            /* We found it */
            matching_builtin_script = found;
        }
    }
    if (matching_builtin_script) {
        has_script_source = true;
        script_source = str2wcstring(matching_builtin_script->def);
    }
    
    if (! has_script_source)
    {
        /* Iterate over path searching for suitable completion files */
        for( i=0; i<path_list.size(); i++ )
        {
            wcstring next = path_list.at(i);
            wcstring path = next + L"/" + cmd + L".fish";

            const file_access_attempt_t access = access_file(path, R_OK);
            if (access.accessible) {
                /* Found it! */
                found_file = true;
                
                /* Now we're actually going to take the lock. */
                scoped_lock locker(lock);
                autoload_function_t *func = this->get_node(cmd);
                
                /* Generate the source if we need to load it */
                bool need_to_load_function = really_load && (func == NULL || func->access.mod_time == access.mod_time || ! func->is_loaded);
                if (need_to_load_function) {
                
                    /* Generate the script source */
                    wcstring esc = escape_string(path, 1);
                    script_source = L". " + esc;
                    has_script_source = true;
                    
                    /* Remove any loaded command because we are going to reload it. Note that this will deadlock if command_removed calls back into us. */
                    if (func && func->is_loaded) {
                        command_removed(cmd);
                        func->is_placeholder = false;
                    }
                    
                    /* Mark that we're reloading it */
                    reloaded = true;
                }
                
                /* Create the function if we haven't yet. This does not load it. Do not trigger eviction unless we are actually loading, because we don't want to evict off of the main thread. */
                if (! func) {
                    func = new autoload_function_t(cmd);
                    if (really_load) {
                        this->add_node(func);
                    } else {
                        this->add_node_without_eviction(func);
                    }
                }
                
                /* It's a fiction to say the script is loaded at this point, but we're definitely going to load it down below. */
                if (need_to_load_function) func->is_loaded = true;
                                                                
                /* Unconditionally record our access time */
                func->access = access;

                break;
            }
        }

        /*
          If no file or builtin script was found we insert a placeholder function.
          Later we only research if the current time is at least five seconds later.
          This way, the files won't be searched over and over again.
        */
        if( ! found_file && ! has_script_source )
        {
            scoped_lock locker(lock);
            /* Generate a placeholder */
            autoload_function_t *func = this->get_node(cmd);
            if (! func) {
                func = new autoload_function_t(cmd);
                func->is_placeholder = true;
                if (really_load) {
                    this->add_node(func);
                } else {
                    this->add_node_without_eviction(func);
                }
            }
            func->access.last_checked = time(NULL);
        }
    }
    
    /* If we have a script, either built-in or a file source, then run it */
    if (really_load && has_script_source)
    {
        if( exec_subshell( script_source.c_str(), 0 ) == -1 )
        {
            /*
              Do nothing on failiure
            */
        }

    }

    if (really_load) {
        return reloaded;
    } else {
        return found_file || has_script_source;
    }
}