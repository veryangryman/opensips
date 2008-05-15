/*
 * $Id$
 *
 * presence module - presence server implementation
 *
 * Copyright (C) 2006 Voice Sistem S.R.L.
 *
 * This file is part of openser, a free SIP server.
 *
 * openser is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * openser is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * History:
 * --------
 *  2006-08-15  initial version (anca)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "../../db/db.h"
#include "../../sr_module.h"
#include "../../dprint.h"
#include "../../error.h"
#include "../../ut.h"
#include "../../parser/parse_to.h"
#include "../../parser/parse_uri.h" 
#include "../../parser/parse_content.h"
#include "../../parser/parse_from.h"
#include "../../mem/mem.h"
#include "../../mem/shm_mem.h"
#include "../../usr_avp.h"
#include "../tm/tm_load.h"
#include "../sl/sl_api.h"
#include "../../pt.h"
#include "../../mi/mi.h"
#include "../pua/hash.h"
#include "publish.h"
#include "subscribe.h"
#include "event_list.h"
#include "bind_presence.h"
#include "notify.h"

MODULE_VERSION

#define S_TABLE_VERSION  3
#define P_TABLE_VERSION  2
#define ACTWATCH_TABLE_VERSION 9

char *log_buf = NULL;
static int clean_period=100;

/* database connection */
db_con_t *pa_db = NULL;
db_func_t pa_dbf;
str presentity_table= str_init("presentity");
str active_watchers_table = str_init("active_watchers");
str watchers_table= str_init("watchers");

int use_db=1;
str server_address= {0, 0};
evlist_t* EvList= NULL;

/* to tag prefix */
char* to_tag_pref = "10";

/* TM bind */
struct tm_binds tmb;
/* SL bind */
struct sl_binds slb;

/** module functions */

static int mod_init(void);
static int child_init(int);
static void destroy(void);
int stored_pres_info(struct sip_msg* msg, char* pres_uri, char* s);
static int fixup_presence(void** param, int param_no);
static struct mi_root* mi_refreshWatchers(struct mi_root* cmd, void* param);
static int update_pw_dialogs(subs_t* subs, unsigned int hash_code, subs_t** subs_array);
int update_watchers_status(str pres_uri, pres_ev_t* ev, str* rules_doc);
static int mi_child_init(void);

int counter =0;
int pid = 0;
char prefix='a';
int startup_time=0;
str db_url = {0, 0};
int expires_offset = 0;
int max_expires= 3600;
int shtable_size= 9;
shtable_t subs_htable= NULL;
int fallback2db= 0;
int sphere_enable= 0;

int phtable_size= 9;
phtable_t* pres_htable;

static cmd_export_t cmds[]=
{
	{"handle_publish",  (cmd_function)handle_publish,  0,    0,         0, REQUEST_ROUTE},
	{"handle_publish",  (cmd_function)handle_publish,  1,fixup_presence,0, REQUEST_ROUTE},
	{"handle_subscribe",(cmd_function)handle_subscribe,0,     0,        0, REQUEST_ROUTE},
	{"bind_presence", (cmd_function)bind_presence,     1,     0,        0,  0},
	{0,                     0,                         0,     0,        0,  0}
};

static param_export_t params[]={
	{ "db_url",                 STR_PARAM, &db_url.s},
	{ "presentity_table",       STR_PARAM, &presentity_table.s},
	{ "active_watchers_table",  STR_PARAM, &active_watchers_table.s},
	{ "watchers_table",         STR_PARAM, &watchers_table.s},
	{ "clean_period",           INT_PARAM, &clean_period },
	{ "to_tag_pref",            STR_PARAM, &to_tag_pref },
	{ "expires_offset",         INT_PARAM, &expires_offset },
	{ "max_expires",            INT_PARAM, &max_expires },
	{ "server_address",         STR_PARAM, &server_address.s},
	{ "subs_htable_size",       INT_PARAM, &shtable_size},
	{ "pres_htable_size",       INT_PARAM, &phtable_size},
	{ "fallback2db",            INT_PARAM, &fallback2db},
	{ "enable_sphere_check",    INT_PARAM, &sphere_enable},
    {0,0,0}
};

static mi_export_t mi_cmds[] = {
	{ "refreshWatchers", mi_refreshWatchers,    0,  0,  mi_child_init},
	{  0,                0,                     0,  0,  0}
};

/** module exports */
struct module_exports exports= {
	"presence",					/* module name */
	DEFAULT_DLFLAGS,			/* dlopen flags */
	cmds,						/* exported functions */
	params,						/* exported parameters */
	0,							/* exported statistics */
	mi_cmds,   					/* exported MI functions */
	0,							/* exported pseudo-variables */
	0,							/* extra processes */
	mod_init,					/* module initialization function */
	(response_function) 0,      /* response handling function */
	(destroy_function) destroy, /* destroy function */
	child_init                  /* per-child init function */
};

/**
 * init module function
 */
static int mod_init(void)
{
	db_url.len = db_url.s ? strlen(db_url.s) : 0;
	LM_DBG("db_url=%s/%d/%p\n", ZSW(db_url.s), db_url.len,db_url.s);
	presentity_table.len = strlen(presentity_table.s);
	active_watchers_table.len = strlen(active_watchers_table.s);
	watchers_table.len = strlen(watchers_table.s);
	int ver = 0;

	LM_NOTICE("initializing module ...\n");

	if(db_url.s== NULL)
	{
		use_db= 0;
		LM_DBG("presence module used for library purpose only\n");
		EvList= init_evlist();
		if(!EvList)
		{
			LM_ERR("unsuccessful initialize event list\n");
			return -1;
		}
		return 0;

	}

	if(expires_offset<0)
		expires_offset = 0;
	
	if(to_tag_pref==NULL || strlen(to_tag_pref)==0)
		to_tag_pref="10";

	if(max_expires<= 0)
		max_expires = 3600;

	if(server_address.s== NULL)
		LM_DBG("server_address parameter not set in configuration file\n");
	
	if(server_address.s)
		server_address.len= strlen(server_address.s);
	else
		server_address.len= 0;

	/* load SL API */
	if(load_sl_api(&slb)==-1)
	{
		LM_ERR("can't load sl functions\n");
		return -1;
	}

	/* load all TM stuff */
	if(load_tm_api(&tmb)==-1)
	{
		LM_ERR("can't load tm functions\n");
		return -1;
	}
	
	/* binding to database module  */
	if (db_bind_mod(&db_url, &pa_dbf))
	{
		LM_ERR("Database module not found\n");
		return -1;
	}
	

	if (!DB_CAPABILITY(pa_dbf, DB_CAP_ALL))
	{
		LM_ERR("Database module does not implement all functions"
				" needed by presence module\n");
		return -1;
	}

	pa_db = pa_dbf.init(&db_url);
	if (!pa_db)
	{
		LM_ERR("connecting to database failed\n");
		return -1;
	}
	
	/*verify table version */

	ver = db_table_version(&pa_dbf, pa_db, &presentity_table);
	if(ver!=P_TABLE_VERSION)
	{
		LM_ERR("Wrong version v%d for table <%.*s>, need v%d\n", 
				ver, presentity_table.len, presentity_table.s , P_TABLE_VERSION);
		return -1;
	}
	
	ver = db_table_version(&pa_dbf, pa_db, &active_watchers_table);
	if(ver!=ACTWATCH_TABLE_VERSION)
	{
		LM_ERR("Wrong version v%d for table <%.*s>, need v%d\n", 
				ver, active_watchers_table.len, active_watchers_table.s,
				ACTWATCH_TABLE_VERSION);
		return -1;
	}

	ver = db_table_version(&pa_dbf, pa_db, &watchers_table);
	if(ver!=S_TABLE_VERSION)
	{
		LM_ERR("Wrong version v%d for table <%.*s>, need v%d\n",
				ver, watchers_table.len, watchers_table.s, S_TABLE_VERSION);
		return -1;
	}

	EvList= init_evlist();
	if(!EvList)
	{
		LM_ERR("initializing event list\n");
		return -1;
	}

	if(clean_period<=0)
	{
		LM_DBG("wrong clean_period \n");
		return -1;
	}

	if(shtable_size< 1)
		shtable_size= 512;
	else
		shtable_size= 1<< shtable_size;

	subs_htable= new_shtable(shtable_size);
	if(subs_htable== NULL)
	{
		LM_ERR(" initializing subscribe hash table\n");
		return -1;
	}
	if(restore_db_subs()< 0)
	{
		LM_ERR("restoring subscribe info from database\n");
		return -1;
	}

	if(phtable_size< 1)
		phtable_size= 256;
	else
		phtable_size= 1<< phtable_size;

	pres_htable= new_phtable();
	if(pres_htable== NULL)
	{
		LM_ERR("initializing presentity hash table\n");
		return -1;
	}
	if(pres_htable_restore()< 0)
	{
		LM_ERR("filling in presentity hash table from database\n");
		return -1;
	}

	startup_time = (int) time(NULL);
	
	register_timer(msg_presentity_clean, 0, clean_period);
	
	register_timer(msg_watchers_clean, 0, clean_period);
	
	register_timer(timer_db_update, 0, clean_period);

	if(pa_db)
		pa_dbf.close(pa_db);
	pa_db = NULL;

	return 0;
}

/**
 * Initialize children
 */
static int child_init(int rank)
{
	LM_NOTICE("init_child [%d]  pid [%d]\n", rank, getpid());

	pid = my_pid();
	
	if(use_db== 0)
		return 0;

	if (pa_dbf.init==0)
	{
		LM_CRIT("child_init: database not bound\n");
		return -1;
	}
	pa_db = pa_dbf.init(&db_url);
	if (!pa_db)
	{
		LM_ERR("child %d: unsuccessful connecting to database\n", rank);
		return -1;
	}
	
	if (pa_dbf.use_table(pa_db, &presentity_table) < 0)  
	{
		LM_ERR( "child %d:unsuccessful use_table presentity_table\n", rank);
		return -1;
	}

	if (pa_dbf.use_table(pa_db, &active_watchers_table) < 0)  
	{
		LM_ERR( "child %d:unsuccessful use_table active_watchers_table\n",
				rank);
		return -1;
	}

	if (pa_dbf.use_table(pa_db, &watchers_table) < 0)  
	{
		LM_ERR( "child %d:unsuccessful use_table watchers_table\n", rank);
		return -1;
	}

	LM_DBG("child %d: Database connection opened successfully\n", rank);
	
	return 0;
}

static int mi_child_init(void)
{
	if(use_db== 0)
		return 0;




	if (pa_dbf.init==0)
	{
		LM_CRIT("database not bound\n");
		return -1;
	}
	pa_db = pa_dbf.init(&db_url);
	if (!pa_db)
	{
		LM_ERR("connecting database\n");
		return -1;
	}
	
	if (pa_dbf.use_table(pa_db, &presentity_table) < 0)
	{
		LM_ERR( "unsuccessful use_table presentity_table\n");
		return -1;
	}

	if (pa_dbf.use_table(pa_db, &active_watchers_table) < 0)
	{
		LM_ERR( "unsuccessful use_table active_watchers_table\n");
		return -1;
	}

	if (pa_dbf.use_table(pa_db, &watchers_table) < 0)
	{
		LM_ERR( "unsuccessful use_table watchers_table\n");
		return -1;
	}

	LM_DBG("Database connection opened successfully\n");
	return 0;
}


/*
 * destroy function
 */
static void destroy(void)
{
	LM_NOTICE("destroy module ...\n");

	if(subs_htable && pa_db)
		timer_db_update(0, 0);

	if(subs_htable)
		destroy_shtable(subs_htable, shtable_size);
	
	if(pres_htable)
		destroy_phtable();

	if(pa_db && pa_dbf.close)
		pa_dbf.close(pa_db);
	
	destroy_evlist();
}

static int fixup_presence(void** param, int param_no)
{
 	pv_elem_t *model;
	str s;
 	if(*param)
 	{
		s.s = (char*)(*param); s.len = strlen(s.s);
 		if(pv_parse_format(&s, &model)<0)
 		{
 			LM_ERR( "wrong format[%s]\n",(char*)(*param));
 			return E_UNSPEC;
 		}
 
 		*param = (void*)model;
 		return 0;
 	}
 	LM_ERR( "null format\n");
 	return E_UNSPEC;
}

/* 
 *  mi cmd: refreshWatchers
 *			<presentity_uri> 
 *			<event>
 *          <refresh_type> // can be:  = 0 -> watchers autentification type or
 *									  != 0 -> publish type //		   
 *		* */

static struct mi_root* mi_refreshWatchers(struct mi_root* cmd, void* param)
{
	struct mi_node* node= NULL;
	str pres_uri, event;
	struct sip_uri uri;
	pres_ev_t* ev;
	str* rules_doc= NULL;
	int result;
	unsigned int refresh_type;

	LM_DBG("start\n");
	
	node = cmd->node.kids;
	if(node == NULL)
		return 0;

	/* Get presentity URI */
	pres_uri = node->value;
	if(pres_uri.s == NULL || pres_uri.len== 0)
	{
		LM_ERR( "empty uri\n");
		return init_mi_tree(404, "Empty presentity URI", 20);
	}
	
	node = node->next;
	if(node == NULL)
		return 0;
	event= node->value;
	if(event.s== NULL || event.len== 0)
	{
		LM_ERR( "empty event parameter\n");
		return init_mi_tree(400, "Empty event parameter", 21);
	}
	LM_DBG("event '%.*s'\n",  event.len, event.s);
	
	node = node->next;
	if(node == NULL)
		return 0;
	if(node->value.s== NULL || node->value.len== 0)
	{
		LM_ERR( "empty event parameter\n");
		return init_mi_tree(400, "Empty event parameter", 21);		
	}
	if(str2int(&node->value, &refresh_type)< 0)
	{
		LM_ERR("converting string to int\n");
		goto error;
	}

	if(node->next!= NULL)
	{
		LM_ERR( "Too many parameters\n");
		return init_mi_tree(400, "Too many parameters", 19);
	}

	ev= contains_event(&event, NULL);
	if(ev== NULL)
	{
		LM_ERR( "wrong event parameter\n");
		return 0;
	}
	
	if(refresh_type== 0) /* if a request to refresh watchers authorization*/
	{
		if(ev->get_rules_doc== NULL)
		{
			LM_ERR("wrong request for a refresh watchers authorization status"
					"for an event that does not require authorization\n");
			goto error;
		}
		
		if(parse_uri(pres_uri.s, pres_uri.len, &uri)< 0)
		{
			LM_ERR( "parsing uri\n");
			goto error;
		}

		result= ev->get_rules_doc(&uri.user,&uri.host,&rules_doc);
		if(result< 0 || rules_doc==NULL || rules_doc->s== NULL)
		{
			LM_ERR( "no rules doc found for the user\n");
			goto error;
		}
	
		if(update_watchers_status(pres_uri, ev, rules_doc)< 0)
		{
			LM_ERR("failed to update watchers\n");
			goto error;
		}

		pkg_free(rules_doc->s);
		pkg_free(rules_doc);
		rules_doc = NULL;

	}
	else     /* if a request to refresh Notified info */
	{
		if(query_db_notify(&pres_uri, ev, NULL)< 0)
		{
			LM_ERR("sending Notify requests\n");
			goto error;
		}

	}
		
	return init_mi_tree(200, "OK", 2);

error:
	if(rules_doc)
	{
		if(rules_doc->s)
			pkg_free(rules_doc->s);
		pkg_free(rules_doc);
	}
	return 0;
}


int pres_update_status(subs_t subs, str reason, db_key_t* query_cols,
        db_val_t* query_vals, int n_query_cols, subs_t** subs_array)
{
	db_key_t update_cols[5];
	db_val_t update_vals[5];
	int n_update_cols= 0;
	int u_status_col, u_reason_col, q_wuser_col, q_wdomain_col;
	int status;
	query_cols[q_wuser_col=n_query_cols]= &str_watcher_username_col;
	query_vals[n_query_cols].nul= 0;
	query_vals[n_query_cols].type= DB_STR;
	n_query_cols++;

	query_cols[q_wdomain_col=n_query_cols]= &str_watcher_domain_col;
	query_vals[n_query_cols].nul= 0;
	query_vals[n_query_cols].type= DB_STR;
	n_query_cols++;

	update_cols[u_status_col= n_update_cols]= &str_status_col;
	update_vals[u_status_col].nul= 0;
	update_vals[u_status_col].type= DB_INT;
	n_update_cols++;

	update_cols[u_reason_col= n_update_cols]= &str_reason_col;
	update_vals[u_reason_col].nul= 0;
	update_vals[u_reason_col].type= DB_STR;
	n_update_cols++;

	status= subs.status;
	if(subs.event->get_auth_status(&subs)< 0)
	{
		LM_ERR( "getting status from rules document\n");
		return -1;
	}
	LM_DBG("subs.status= %d\n", subs.status);
	if(get_status_str(subs.status)== NULL)
	{
		LM_ERR("wrong status: %d\n", subs.status);
		return -1;
	}

	if(subs.status!= status || reason.len!= subs.reason.len ||
		(reason.s && subs.reason.s && strncmp(reason.s, subs.reason.s,
											  reason.len)))
	{
		/* update in watchers_table */
		query_vals[q_wuser_col].val.str_val= subs.from_user; 
		query_vals[q_wdomain_col].val.str_val= subs.from_domain; 

		update_vals[u_status_col].val.int_val= subs.status;
		update_vals[u_reason_col].val.str_val= subs.reason;
		
		if (pa_dbf.use_table(pa_db, &watchers_table) < 0) 
		{
			LM_ERR( "in use_table\n");
			return -1;
		}

		if(pa_dbf.update(pa_db, query_cols, 0, query_vals, update_cols,
					update_vals, n_query_cols, n_update_cols)< 0)
		{
			LM_ERR( "in sql update\n");
			return -1;
		}
		/* save in the list all affected dialogs */
		/* if status switches to terminated -> delete dialog */
		if(update_pw_dialogs(&subs, subs.db_flag, subs_array)< 0)
		{
			LM_ERR( "extracting dialogs from [watcher]=%.*s@%.*s to"
				" [presentity]=%.*s\n",	subs.from_user.len, subs.from_user.s,
				subs.from_domain.len, subs.from_domain.s, subs.pres_uri.len,
				subs.pres_uri.s);
			return -1;
		}
	}
    return 0;
}

int pres_db_delete_status(subs_t* s)
{
    int n_query_cols= 0;
    db_key_t query_cols[5];
    db_val_t query_vals[5];

    if (pa_dbf.use_table(pa_db, &active_watchers_table) < 0) 
    {
        LM_ERR("sql use table failed\n");
        return -1;
    }

    query_cols[n_query_cols]= &str_event_col;
    query_vals[n_query_cols].nul= 0;
    query_vals[n_query_cols].type= DB_STR;
    query_vals[n_query_cols].val.str_val= s->event->name ;
    n_query_cols++;

    query_cols[n_query_cols]= &str_presentity_uri_col;
    query_vals[n_query_cols].nul= 0;
    query_vals[n_query_cols].type= DB_STR;
    query_vals[n_query_cols].val.str_val= s->pres_uri;
    n_query_cols++;

    query_cols[n_query_cols]= &str_watcher_username_col;
    query_vals[n_query_cols].nul= 0;
    query_vals[n_query_cols].type= DB_STR;
    query_vals[n_query_cols].val.str_val= s->from_user;
    n_query_cols++;

    query_cols[n_query_cols]= &str_watcher_domain_col;
    query_vals[n_query_cols].nul= 0;
    query_vals[n_query_cols].type= DB_STR;
    query_vals[n_query_cols].val.str_val= s->from_domain;
    n_query_cols++;

    if(pa_dbf.delete(pa_db, query_cols, 0, query_vals, n_query_cols)< 0)
    {
        LM_ERR("sql delete failed\n");
        return -1;
    }
    return 0;

}

int update_watchers_status(str pres_uri, pres_ev_t* ev, str* rules_doc)
{
	subs_t subs;
	db_key_t query_cols[6], result_cols[5];
	db_val_t query_vals[6];
	int n_result_cols= 0, n_query_cols = 0;
	db_res_t* result= NULL;
	db_row_t *row;
	db_val_t *row_vals ;
	int i;
	str w_user, w_domain, reason= {0, 0};
	unsigned int status;
	int status_col, w_user_col, w_domain_col, reason_col;
	subs_t* subs_array= NULL,* s;
	unsigned int hash_code;
	int err_ret= -1;
	int n= 0;

	typedef struct ws
	{
		int status;
		str reason;
		str w_user;
		str w_domain;
	}ws_t;
	ws_t* ws_list= NULL;

    LM_DBG("start\n");

	if(ev->content_type.s== NULL)
	{
		ev= contains_event(&ev->name, NULL);
		if(ev== NULL)
		{
			LM_ERR("wrong event parameter\n");
			return 0;
		}
	}

	subs.pres_uri= pres_uri;
	subs.event= ev;
	subs.auth_rules_doc= rules_doc;

	/* update in watchers_table */
	query_cols[n_query_cols]= &str_presentity_uri_col;
	query_vals[n_query_cols].nul= 0;
	query_vals[n_query_cols].type= DB_STR;
	query_vals[n_query_cols].val.str_val= pres_uri;
	n_query_cols++;

	query_cols[n_query_cols]= &str_event_col;
	query_vals[n_query_cols].nul= 0;
	query_vals[n_query_cols].type= DB_STR;
	query_vals[n_query_cols].val.str_val= ev->name;
	n_query_cols++;

	result_cols[status_col= n_result_cols++]= &str_status_col;
	result_cols[reason_col= n_result_cols++]= &str_reason_col;
	result_cols[w_user_col= n_result_cols++]= &str_watcher_username_col;
	result_cols[w_domain_col= n_result_cols++]= &str_watcher_domain_col;

	if (pa_dbf.use_table(pa_db, &watchers_table) < 0) 
	{
		LM_ERR( "in use_table\n");
		goto done;
	}

	if(pa_dbf.query(pa_db, query_cols, 0, query_vals, result_cols,n_query_cols,
				n_result_cols, 0, &result)< 0)
	{
		LM_ERR( "in sql query\n");
		goto done;
	}
	if(result== NULL)
		return 0;

	if(result->n<= 0)
	{
		err_ret= 0;
		goto done;
	}

    LM_DBG("found %d record-uri in watchers_table\n", result->n);
	hash_code= core_hash(&pres_uri, &ev->name, shtable_size);
	subs.db_flag= hash_code;

    /*must do a copy as sphere_check requires database queries */
	if(sphere_enable)
	{
        n= result->n;
		ws_list= (ws_t*)pkg_malloc(n * sizeof(ws_t));
		if(ws_list== NULL)
		{
			LM_ERR("No more private memory\n");
			goto done;
		}
		memset(ws_list, 0, n * sizeof(ws_t));

		for(i= 0; i< result->n ; i++)
		{
			row= &result->rows[i];
			row_vals = ROW_VALUES(row);

			status= row_vals[status_col].val.int_val;
	
			reason.s= (char*)row_vals[reason_col].val.string_val;
			reason.len= reason.s?strlen(reason.s):0;

			w_user.s= (char*)row_vals[w_user_col].val.string_val;
			w_user.len= strlen(w_user.s);

			w_domain.s= (char*)row_vals[w_domain_col].val.string_val;
			w_domain.len= strlen(w_domain.s);

			if(reason.len)
			{
				ws_list[i].reason.s = (char*)pkg_malloc(reason.len* sizeof(char));
				if(ws_list[i].reason.s== NULL)
				{  
					LM_ERR("No more private memory\n");
					goto done;
				}
				memcpy(ws_list[i].reason.s, reason.s, reason.len);
				ws_list[i].reason.len= reason.len;
			}
			else
				ws_list[i].reason.s= NULL;
            
			ws_list[i].w_user.s = (char*)pkg_malloc(w_user.len* sizeof(char));
			if(ws_list[i].w_user.s== NULL)
			{
				LM_ERR("No more private memory\n");
				goto done;

			}
			memcpy(ws_list[i].w_user.s, w_user.s, w_user.len);
			ws_list[i].w_user.len= w_user.len;
		
			 ws_list[i].w_domain.s = (char*)pkg_malloc(w_domain.len* sizeof(char));
			if(ws_list[i].w_domain.s== NULL)
			{
				LM_ERR("No more private memory\n");
				goto done;
			}
			memcpy(ws_list[i].w_domain.s, w_domain.s, w_domain.len);
			ws_list[i].w_domain.len= w_domain.len;
			
			ws_list[i].status= status;
		}

		pa_dbf.free_result(pa_db, result);
		result= NULL;

		for(i=0; i< n; i++)
		{
			subs.from_user = ws_list[i].w_user;
			subs.from_domain = ws_list[i].w_domain;
			subs.status = ws_list[i].status;
			memset(&subs.reason, 0, sizeof(str));

			if( pres_update_status(subs, reason, query_cols, query_vals,
					n_query_cols, &subs_array)< 0)
			{
				LM_ERR("failed to update watcher status\n");
				goto done;
			}

		}
        
		for(i=0; i< n; i++)
		{
			pkg_free(ws_list[i].w_user.s);
			pkg_free(ws_list[i].w_domain.s);
			if(ws_list[i].reason.s)
				pkg_free(ws_list[i].reason.s);
		}
		ws_list= NULL;

		goto send_notify;

	}
	
	for(i = 0; i< result->n; i++)
	{
		row= &result->rows[i];
		row_vals = ROW_VALUES(row);

		status= row_vals[status_col].val.int_val;
	
		reason.s= (char*)row_vals[reason_col].val.string_val;
		reason.len= reason.s?strlen(reason.s):0;

		w_user.s= (char*)row_vals[w_user_col].val.string_val;
		w_user.len= strlen(w_user.s);

		w_domain.s= (char*)row_vals[w_domain_col].val.string_val;
		w_domain.len= strlen(w_domain.s);

		subs.from_user= w_user;
		subs.from_domain= w_domain;
		subs.status= status;
		memset(&subs.reason, 0, sizeof(str));

 		if( pres_update_status(subs,reason, query_cols, query_vals,
					n_query_cols, &subs_array)< 0)
		{
			LM_ERR("failed to update watcher status\n");
			goto done;
		}
    }

	pa_dbf.free_result(pa_db, result);
	result= NULL;

send_notify:

	s= subs_array;

	while(s)
	{

		if(notify(s, NULL, NULL, 0)< 0)
		{
			LM_ERR( "sending Notify request\n");
			goto done;
		}

        /* delete from database also */
        if(s->status== TERMINATED_STATUS)
        {
            if(pres_db_delete_status(s)<0)
            {
                err_ret= -1;
                LM_ERR("failed to delete terminated dialog from database\n");
                goto done;
            }
        }

        s= s->next;
	}

	free_subs_list(subs_array, PKG_MEM_TYPE, 0);
	return 0;

done:
	if(result)
		pa_dbf.free_result(pa_db, result);
	free_subs_list(subs_array, PKG_MEM_TYPE, 0);
	if(ws_list)
	{
		for(i= 0; i< n; i++)
		{
			if(ws_list[i].w_user.s)
				pkg_free(ws_list[i].w_user.s);
			else
				break;
			if(ws_list[i].w_domain.s)
				pkg_free(ws_list[i].w_domain.s);
			if(ws_list[i].reason.s)
				pkg_free(ws_list[i].reason.s);
		}
	}
	return err_ret;
}

static int update_pw_dialogs(subs_t* subs, unsigned int hash_code, subs_t** subs_array)
{
	subs_t* s, *ps, *cs;
	int i= 0;

    LM_DBG("start\n");
	lock_get(&subs_htable[hash_code].lock);
	
    ps= subs_htable[hash_code].entries;
	
	while(ps && ps->next)
	{
		s= ps->next;

		if(s->event== subs->event && s->pres_uri.len== subs->pres_uri.len &&
			s->from_user.len== subs->from_user.len && 
			s->from_domain.len==subs->from_domain.len &&
			strncmp(s->pres_uri.s, subs->pres_uri.s, subs->pres_uri.len)== 0 &&
			strncmp(s->from_user.s, subs->from_user.s, s->from_user.len)== 0 &&
			strncmp(s->from_domain.s,subs->from_domain.s,s->from_domain.len)==0)
		{
			i++;
			s->status= subs->status;
			s->reason= subs->reason;
			s->db_flag= UPDATEDB_FLAG;

			cs= mem_copy_subs(s, PKG_MEM_TYPE);
			if(cs== NULL)
			{
				LM_ERR( "copying subs_t stucture\n");
                lock_release(&subs_htable[hash_code].lock);
                return -1;
			}
			cs->expires-= (int)time(NULL);
			cs->next= (*subs_array);
			(*subs_array)= cs;
			if(subs->status== TERMINATED_STATUS)
			{
				ps->next= s->next;
				shm_free(s->contact.s);
                shm_free(s);
                LM_DBG(" deleted terminated dialog from hash table\n");
            }
			else
				ps= s;

			printf_subs(cs);
		}
		else
			ps= s;
	}
	
    LM_DBG("found %d matching dialogs\n", i);
    lock_release(&subs_htable[hash_code].lock);
	
    return 0;
}
