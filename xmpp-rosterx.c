/*
 * XMPP Roster Item Exchange plugin for Pidgin/libpurple
 * 
 * Copyright (C) 2017  Dustin Gathmann
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 *
 */

#include <glib.h>

#include "internal.h"
#include "debug.h"
#include "notify.h"
#include "request.h"
#include "plugin.h"
#include "version.h"

#include "xmpp-rosterx.h"

#define equals(X, Y)     (g_strcmp0(X, Y) == 0)  /* g_str_equal(), you suck */


#define PLUGIN_ID        "core-dzzinstant-rosterx"
#define PLUGIN_AUTHOR    "<dzsoftware@posteo.org>"
#define PLUGIN_VERSION   "0.0.5"
#define PLUGIN_HOMEPAGE  "https://github.com/dzzinstant/pidgin-rosterx-plugin"

#define NS_ROSTERX       "http://jabber.org/protocol/rosterx"
#define NS_XMPP_STANZAS  "urn:ietf:params:xml:ns:xmpp-stanzas"
#define NS_CARBONS       "urn:xmpp:carbons:2"

//#define GROUPNAME_DEFAULT _("Buddies")
#define GROUPNAME_DEFAULT "RosterX Suggestions"

PurplePlugin  *rosterx_plugin = NULL;


typedef struct _Item Item;
struct _Item {
	char *jid;
	char *alias;
	GList *entries; /* entries are char* */
};

typedef struct _AuxData AuxData;
struct _AuxData {
	PurpleConnection *pc;
	char *target_jid;
};

typedef gboolean (*BuddyConditionFunc)(PurpleBuddy *);
typedef gboolean (*ItemConditionFunc)(Item *, PurpleAccount *);

static int global_entry_count = 0;
static int global_auxdata_count = 0;

static AuxData*
auxdata_new(PurpleConnection *pc)
{
	AuxData *aux = g_new0(AuxData, 1);
	aux->pc = pc;

	purple_debug_misc(PLUGIN_ID, "auxdata_new(): now %d auxdata\n", ++global_auxdata_count);
	return aux;
}

static void
auxdata_destroy(AuxData *aux)
{
	g_free(aux->target_jid);
	g_free(aux);

	purple_debug_misc(PLUGIN_ID, "auxdata_destroy(): now %d auxdata\n", --global_auxdata_count);
}

static Item*
item_new(const char *key, const char *value)
{
	Item *item = g_new0(Item, 1);
	item->jid = g_strdup(key);
	item->alias = g_strdup(value);

	purple_debug_misc(PLUGIN_ID, "item_new(): now %d entries\n", ++global_entry_count);
	return item;
}

static void
item_destroy(gpointer _item)
{
	Item *item = (Item *) _item;
	if (!item)
		return;

	g_free(item->jid);
	g_free(item->alias);
	g_list_free_full(item->entries, g_free);
	g_free(item);

	purple_debug_misc(PLUGIN_ID, "item_destroy(): now %d entries\n", --global_entry_count);
}

static void
item_add_group(Item *item, const char *groupname)
{
	if (!g_list_find_custom(item->entries, groupname, (GCompareFunc) g_strcmp0)) {
		item->entries = g_list_append(item->entries, g_strdup(groupname));
		// purple_debug_misc(PLUGIN_ID, "item_add_group(): adding group %s to %s\n", groupname, item->jid);
	}
}

static Item*
item_new_from_xitem(xmlnode *xitem)
{
	Item *item;
	xmlnode *xgroup;
	const char *jid = xmlnode_get_attrib(xitem, "jid");
	const char *alias = xmlnode_get_attrib(xitem, "name");

	if (!jid) {
		purple_debug_warning(PLUGIN_ID, "XEP-0144 MUST: Requested exchange action has no jid, ignoring!\n");
		return NULL;
	}
	item = item_new(jid, alias);

	for (xgroup = xmlnode_get_child(xitem, "group"); xgroup; xgroup = xmlnode_get_next_twin(xgroup)) {
		const char *groupname = xmlnode_get_data(xgroup);

		if (groupname)
			item_add_group(item, groupname);
	}
	return item;
}

/*
 * Itemlist methods
 */
static Item *
itemlist_find_by_jid(GList *itemlist, const char *jid)
{
	GList *l;

	for (l = g_list_first(itemlist); l; l = g_list_next(l)) {
		Item *data = (Item *) l->data;

		if (equals(jid, data->jid))
			return data;
	}
	return NULL;
}

static void
itemlist_destroy(GList *list)
{
	g_list_free_full(list, item_destroy);
}

static gboolean
_item_is_not_in_roster(Item *item, PurpleAccount *account)
{
	g_return_val_if_fail(item, TRUE);

	return (purple_find_buddy(account, item->jid) == NULL);
}

/* NOTE: Steals entries and deletes original grouplist */
static GList *
itemlist_filter(GList *list, ItemConditionFunc _item_condition, AuxData *aux)
{
	GList *result = NULL, *ignored = NULL;
	PurpleAccount *account = purple_connection_get_account(aux->pc);
	GList *l = g_list_first(list);

	do {
		Item *item = (Item *) l->data;

		if (_item_condition(item, account)) {
			// purple_debug_misc(PLUGIN_ID, "grouplist_filter(): Item %s to filtered list\n", item->jid);
			result = g_list_prepend(result, item);
		} else {
			// purple_debug_misc(PLUGIN_ID, "grouplist_filter(): Item %s will be ignored\n", item->jid);
			ignored = g_list_prepend(ignored, item);
		}
		l = g_list_remove(l, item);
	} while (l);

	itemlist_destroy(ignored);
	return g_list_reverse(result);
}

static char*
create_name_from_label(const char *label)
{
	char *delimiter = g_strrstr(label, " <");
	g_return_val_if_fail(delimiter, NULL);

	return g_strndup(label, delimiter - label);
}


/*
 * Data conversion path:
 *
 *   blist  ...> request ---> request  ...> xnode
 *     v            ^            v            ^
 *     v  itemlist  ^            v  itemlist  ^
 *
 *
 *   xnode  ...> searchresults ---> libpurple
 *     v            ^
 *     v  itemlist  ^
 *
 */

static gboolean
_buddy_is_xmpp(PurpleBuddy *b)
{
	const char *protocol_id = purple_account_get_protocol_id(
			purple_buddy_get_account(b));

	return (equals("prpl-jabber", protocol_id) );
}

static GList *
itemlist_new_from_blist(BuddyConditionFunc _buddy_condition)
{
	PurpleBlistNode *node;
	const char *groupname = NULL;
	GList *itemlist = NULL;

	for (node = purple_blist_get_root(); node; node = purple_blist_node_next(node, TRUE) ) {

		if (PURPLE_BLIST_NODE_IS_GROUP(node)) {
			PurpleGroup *g = (PurpleGroup *) node;

			groupname = purple_group_get_name(g);
			g_warn_if_fail(groupname);

			// purple_debug_misc(PLUGIN_ID, "blist -> itemlist: group %s\n", groupname);
		}
		else if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
			PurpleBuddy *b = (PurpleBuddy *) node;

			if (_buddy_condition(b)) {
				const char *jid = purple_buddy_get_name(b);
				const char *alias = purple_buddy_get_alias(b);
				Item *item = itemlist_find_by_jid(itemlist, jid);

				if (!item) {
					item = item_new(jid, alias);
					itemlist = g_list_append(itemlist, item);
					// purple_debug_misc(PLUGIN_ID, "blist -> itemlist: new buddy %s\n", jid);
				}
				item_add_group(item, groupname);
			}
		}
	}
	return itemlist;
}


static PurpleRequestFieldGroup*
find_request_group_by_title(PurpleRequestFields *request, const char *title)
{
	PurpleRequestFieldGroup *rgroup;
	GList *g;
	GList *rgroups = purple_request_fields_get_groups(request);

	for (g = g_list_first(rgroups); g; g = g_list_next(g)) {
		rgroup = (PurpleRequestFieldGroup *) g->data;

		if (equals(title, purple_request_field_group_get_title(rgroup)))
			return rgroup;
	}
	return NULL;
}

static PurpleRequestFields *
request_new_from_itemlist(GList *itemlist)
{
	PurpleRequestFields *request = purple_request_fields_new();
	PurpleRequestFieldGroup *rgroup;
	PurpleRequestField *field;
	GList *i, *g;

	for (i = g_list_first(itemlist); i; i = g_list_next(i)) {
		Item *item = (Item *) i->data;
		const char *jid = item->jid;
		const char *alias = item->alias;
		char *label = g_strdup_printf("%s <%s>", alias, jid);

		// purple_debug_misc(PLUGIN_ID, "itemlist -> request: jid %s added, label %s\n", jid, label);

		if (!item->entries) /* Item has no groups, so use our default group */
			item->entries = g_list_append(item->entries, GROUPNAME_DEFAULT);

		for (g = g_list_first(item->entries); g; g = g_list_next(g)) {
			const char *groupname = g->data;

			rgroup = find_request_group_by_title(request, groupname);
			if (!rgroup) {
				rgroup = purple_request_field_group_new(groupname);
				purple_request_fields_add_group(request, rgroup);
			}

			field = purple_request_field_bool_new(jid, label, FALSE);
			purple_request_field_group_add_field(rgroup, field);

			purple_debug_misc(PLUGIN_ID, "itemlist -> request: jid %s added group %s\n", jid, groupname);
		}
		g_free(label);
	}
	return request;
}

static GList *
itemlist_new_from_request(PurpleRequestFields *request)
{
	GList *f, *g;
	GList *itemlist = NULL;
	GList *request_groups = purple_request_fields_get_groups(request);
	Item *item;

	for (g = g_list_first(request_groups); g; g = g_list_next(g)) {
		PurpleRequestFieldGroup *request_group = (PurpleRequestFieldGroup *) g->data;
		const char *groupname = purple_request_field_group_get_title(request_group);

		// purple_debug_misc(PLUGIN_ID, "request -> itemlist: entering group field %s\n", groupname);

		for (f = purple_request_field_group_get_fields(request_group); f; f = g_list_next(f)) {
			PurpleRequestField *field = (PurpleRequestField *) f->data;

			if (purple_request_field_bool_get_value(field) == TRUE) {
				const char *jid = purple_request_field_get_id(field);
				char *alias = create_name_from_label(purple_request_field_get_label(field));

				item = itemlist_find_by_jid(itemlist, jid);
				if (!item) {
					// purple_debug_misc(PLUGIN_ID, "request -> itemlist: item %s added to itemlist, username %s\n", jid, alias);
					item = item_new(jid, alias);
					itemlist = g_list_prepend(itemlist, item);
				}

				item_add_group(item, groupname);
				g_free(alias);
			}
		}
	}
	return g_list_reverse(itemlist);
}

static xmlnode*
xnode_new_from_itemlist(GList *itemlist)
{
	xmlnode *xnode;
	GList *i, *g;

	xnode = xmlnode_new("x");
	xmlnode_set_namespace(xnode, NS_ROSTERX);

	for (i = g_list_first(itemlist); i; i = g_list_next(i)) {
		xmlnode *xitem;
		Item *item = (Item *) i->data;

		xitem = xmlnode_new_child(xnode, "item");
		xmlnode_set_attrib(xitem, "action", "add"); /* Only available action for now */
		xmlnode_set_attrib(xitem, "jid", item->jid);
		xmlnode_set_attrib(xitem, "name", item->alias);

		// purple_debug_misc(PLUGIN_ID, "itemlist -> xnode: jid %s added, alias %s\n", item->jid, item->alias);

		for (g = g_list_first(item->entries); g; g = g_list_next(g)) {
			const char *groupname = g->data;
			xmlnode *xgroup = xmlnode_new_child(xitem, "group");

			xmlnode_insert_data(xgroup, groupname, -1);
			// purple_debug_misc(PLUGIN_ID, "itemlist -> xnode: jid %s adding group %s\n", item->jid, groupname);
		}
	}
	return xnode;
}


static GList*
itemlist_new_from_xnode(xmlnode *xnode)
{
	xmlnode *xitem;
	GList *itemlist = NULL;

	for (xitem = xmlnode_get_child(xnode, "item"); xitem; xitem = xmlnode_get_next_twin(xitem)) {
		Item *item = NULL;
		const char *action = xmlnode_get_attrib(xitem, "action");
		const char *jid = xmlnode_get_attrib(xitem, "jid");

		if (!action || equals("add", action)) { /* default action is 'add' */
			if (!itemlist_find_by_jid(itemlist, jid)) {
				item = item_new_from_xitem(xitem);
				itemlist = g_list_prepend(itemlist, item);
			}
		}
		else { /* 'modify' and 'delete' are not implemented */
			purple_debug_warning(PLUGIN_ID,
					"Received unknown Roster exchange action '%s'!\n", action);
		}
	}

	if (!itemlist)
		purple_debug_warning(PLUGIN_ID, "XEP-0144 MUST: Parsed xnode does not contain any items!\n");
	return g_list_reverse(itemlist);
}

/*
 * Searchresult table
 */
static void
add_rosteritem_cb(PurpleConnection *c, GList *row, gpointer userdata)
{
	purple_blist_request_add_buddy(
			purple_connection_get_account(c),
			g_list_nth_data(row, 1), /* JID   */
			g_list_nth_data(row, 2), /* Group */
			g_list_nth_data(row, 0)  /* Alias */
			);
}

static void
add_all_rosteritems_cb(PurpleConnection *c, GList *row, gpointer userdata) 
{
	PurpleNotifySearchResults *results = (PurpleNotifySearchResults *) userdata;
	GList *r;
	g_return_if_fail(results);

	for (r = g_list_first(results->rows); r; r = g_list_next(r)) {
		GList *row = (GList *) r->data;

		add_rosteritem_cb(c, row, userdata);
	}
}

static void
add_row(PurpleNotifySearchResults *rec_items,
		const char *jid, const char *alias, const char *groupname)
{
	GList *item_row = NULL;

	/* Name attribute is optional in rosteritem, so we have to check if
	 * it exists in each rosteritem
	 */
	item_row = g_list_append(item_row, g_strdup(alias ? alias : jid));
	item_row = g_list_append(item_row, g_strdup(jid));
	item_row = g_list_append(item_row, g_strdup(groupname));
	
	purple_notify_searchresults_row_add(rec_items, item_row);
}

static void
searchresults_new_from_itemlist(GList *itemlist, AuxData *aux)
{
	GList *i, *g;
	PurpleNotifySearchResults *rec_items;
	char *rosteritems_title;

	if (!itemlist) {
		purple_debug_info(PLUGIN_ID, "itemlist -> searchresults: resulting itemlist is empty, no action\n");
		return;
	}

	rec_items = purple_notify_searchresults_new();

	purple_notify_searchresults_column_add(rec_items, 
			purple_notify_searchresults_column_new(_("Name")));
	purple_notify_searchresults_column_add(rec_items,
			purple_notify_searchresults_column_new(_("JID")));
	purple_notify_searchresults_column_add(rec_items,
			purple_notify_searchresults_column_new(_("Group")));

	for (i = g_list_first(itemlist); i; i = g_list_next(i)) {
		Item *item = (Item *) i->data;
		const char *jid  = item->jid;
		const char *alias = item->alias;

		if (item->entries) { /* extra verbosity: one row for each group of the item */
			for (g = g_list_first(item->entries); g; g = g_list_next(g)) {
				const char *groupname = g->data;
				add_row(rec_items, jid, alias, groupname);
			}
		} else {
			add_row(rec_items, jid, alias, NULL);
		}
	} /* End of processing one roster item */

	purple_notify_searchresults_button_add(rec_items,
			PURPLE_NOTIFY_BUTTON_ADD, add_rosteritem_cb);

   	/* NOTE: This button is not visible in Pidgin < 3.0.0dev
	 * because of a bug in gtknotify.c */
	purple_notify_searchresults_button_add_labeled(rec_items,
			_("All"), add_all_rosteritems_cb);

	rosteritems_title = g_strdup_printf("User %s has sent you a contact suggestion:",
			aux->target_jid);
	purple_notify_searchresults(
			aux->pc,
			purple_account_get_username(purple_connection_get_account(aux->pc)),
			rosteritems_title,
			NULL,
			rec_items,
			NULL,       /* close callback */
			rec_items   /* userdata */
			);

	g_free(rosteritems_title);
}


/*
 * Jabber helper functions
 */
static char*
generate_next_id()
{
	static guint32 index = 0;

	if (index == 0) {
		do {
			index = g_random_int();
		} while (index == 0);
	}
	return g_strdup_printf("rosterx%x", index++);
}

static char*
create_full_jid(const char *bare_jid, const char *resource)
{
	return g_strdup_printf("%s/%s", bare_jid, resource);
}

static char*
create_bare_jid(const char *jid)
{
	gchar **parts = g_strsplit(jid, "/", 2);
	gchar *bare_jid = g_strdup(parts[0]);

	g_strfreev(parts);
	return bare_jid;
}

static gboolean
jid_is_subscribed(PurpleConnection *pc, const char *jid)
{
	DummyJabberStream *js = purple_connection_get_protocol_data(pc);
	char *bare_jid = create_bare_jid(jid);
	DummyJabberBuddy *jb = g_hash_table_lookup(js->buddies, bare_jid);

	g_free(bare_jid);
	return jb && !(jb->subscription & JABBER_SUB_PENDING) && (jb->subscription & JABBER_SUB_BOTH);
}

static GList *
find_resources(PurpleConnection *pc, const char *jid)
{
	DummyJabberStream *js = purple_connection_get_protocol_data(pc);
	DummyJabberBuddy *jb;
	GList *list = NULL;
	GList *resources;
	char *bare_jid = create_bare_jid(jid);

	jb = g_hash_table_lookup(js->buddies, bare_jid);
	if (jb && jb->resources) {
		for(resources = jb->resources; resources; resources = resources->next) {
			DummyJabberBuddyResource *jbr = resources->data;
			char *resource = jbr->name;

			purple_debug_misc(PLUGIN_ID, "jid=%s has resource %s\n", bare_jid, resource);
			list = g_list_append(list, resource);
		}
	}
	g_free(bare_jid);
	return list;
}

static gboolean
_resource_has_feature(PurpleBuddy *b, const char *resource, const char *namespace)
{
	void *jabber_handle = purple_plugins_find_with_id("prpl-jabber");
	char *full_jid = create_full_jid(purple_buddy_get_name(b), resource);
	gboolean ipc_success;
	int result;

	result = GPOINTER_TO_INT(purple_plugin_ipc_call(jabber_handle,
				"contact_has_feature", &ipc_success,
				purple_buddy_get_account(b),
				full_jid,
				namespace));

	// purple_debug_misc(PLUGIN_ID, "_resource_has_feature(): ns=%s, jid=%s, full=%s, ipc_success=%s, result=%x\n",
	//		namespace, purple_buddy_get_name(b), full_jid, ipc_success ? "yes":"no", result);

	g_free(full_jid);
	return (ipc_success && result);
}

static GList *
find_resources_with_feature(PurpleBuddy *b, const char *namespace)
{
	GList *featured = NULL;
	GList *r;

	r = find_resources(purple_account_get_connection(purple_buddy_get_account(b)),
			purple_buddy_get_name(b));
	for (r = g_list_first(r); r; r = g_list_next(r)) {
		const char *resource = r->data;

		if (_resource_has_feature(b, resource, namespace))
			featured = g_list_prepend(featured, g_strdup(resource));
	}
	return g_list_reverse(featured);
}


/*
 * Generic sending of an iq or message
 */
static void
send_iq(PurpleConnection *pc, const char *full_to, xmlnode *xnode)
{
	xmlnode *iq;
	const char *from = purple_account_get_username(
			purple_connection_get_account(pc));

	iq = xmlnode_new("iq");
	xmlnode_set_attrib(iq, "type", "set");
	xmlnode_set_attrib(iq, "id", generate_next_id());
	xmlnode_set_attrib(iq, "to", full_to);
	xmlnode_set_attrib(iq, "from", from);
	xmlnode_insert_child(iq, xnode);

	// purple_debug_info(PLUGIN_ID, "Sending request iq from '%s' to '%s'...\n", from, full_to);
	purple_signal_emit(purple_connection_get_prpl(pc), "jabber-sending-xmlnode", pc, &iq);

	xmlnode_free(iq);
}

static void
send_message(PurpleConnection *pc, const char *to, xmlnode *xnode, const char *text)
{
	xmlnode *message, *node;
	const char *from = purple_account_get_username(
			purple_connection_get_account(pc));

	message = xmlnode_new("message");
	xmlnode_set_attrib(message, "id", generate_next_id());
	xmlnode_set_attrib(message, "to", to);
	xmlnode_set_attrib(message, "from", from);
	xmlnode_insert_child(message, xnode);

	/* We don't want the message echoed back to our other devices */
	node = xmlnode_new_child(message, "private");
	xmlnode_set_namespace(node, NS_CARBONS);
	if (text) {
		node = xmlnode_new_child(message, "body");
		xmlnode_insert_data(node, text, -1);
	}

	// purple_debug_info(PLUGIN_ID, "Sending request message to '%s'...\n", to);
	purple_signal_emit(purple_connection_get_prpl(pc), "jabber-sending-xmlnode", pc, &message);

	xmlnode_free(message);
}

// TODO: clearer separation between XEP-compliant and mobile-compatible mode
/* 
 * If entity is online, this implementation sends <iq/> requests
 * to _all_ RosterX-capable resources.
 *
 * NOTE: This behaviour is inconsistent
 * with XEP-0144 (5. Recommended Stanza Types).
 * This is because it is not known which resource would be the most
 * adequate to address.
 *
 * If the entity is offline, send a message to the bare jid instead.
 */
static void
send_iqs_or_message(PurpleConnection *pc, const char *to, xmlnode *xnode, const char *text)
{
	PurpleBuddy *b = purple_find_buddy(
			purple_connection_get_account(pc), to);
	GList *resources;
	g_return_if_fail(b);

	resources = g_list_first(find_resources_with_feature(b, NS_ROSTERX));

	if (PURPLE_BUDDY_IS_ONLINE(b) && resources) {
		while (resources) {
			char *full_jid = create_full_jid(to, resources->data);

			purple_debug_info(PLUGIN_ID, "send_iqs_or_message(): <iq/> to=%s\n", full_jid);

			send_iq(pc, full_jid, xmlnode_copy(xnode));

			g_free(full_jid);
			resources = g_list_next(resources);
		}
		xmlnode_free(xnode);

	} else { /* fallback if buddy is offline or has no RosterX resource */
		send_message(pc, to, xnode, text);
	}
}


/*
 * Generate a RosterX suggestion
 */
static char *
create_message_from_itemlist(GList *itemlist, PurpleConnection *pc)
{
	GList *l;
	char *text = g_strdup_printf("%s has sent you a RosterX contact suggestion:\n",
			purple_account_get_name_for_display(purple_connection_get_account(pc)));

	for (l = g_list_first(itemlist); l; l = g_list_next(l)) {
		Item *item = (Item *) l->data;
		char *tmptext = text;

		text = g_strdup_printf("%s+ %s\nxmpp:%s", tmptext,
				item->alias, item->jid);
		g_free(tmptext);
	}
	return text;
}


static void
select_contacts_ok(AuxData *aux, PurpleRequestFields *request)
{
	PurpleConnection *pc = aux->pc;
	GList *itemlist = itemlist_new_from_request(request);

	if (itemlist) {
		xmlnode *xnode = xnode_new_from_itemlist(itemlist);
		const char *to = aux->target_jid;
		char *text = create_message_from_itemlist(itemlist, pc);

		send_iqs_or_message(pc, to, xnode, text);

		g_free(text);
		itemlist_destroy(itemlist);
	}
	auxdata_destroy(aux);
}

static void
select_contacts_cancel(AuxData *aux, PurpleRequestFields *request)
{
	auxdata_destroy(aux);
}

static void
select_contacts(PurpleBlistNode *node, gpointer plugin)
{
	PurpleBuddy *b = (PurpleBuddy *) node;
	PurpleConnection *pc = purple_account_get_connection(purple_buddy_get_account(b));
	PurpleRequestFields *request;
	AuxData *aux;
	GList *itemlist;
	char *tmpstring;

	g_return_if_fail(pc && b);

	aux = auxdata_new(pc);
	aux->target_jid = g_strdup(purple_buddy_get_name(b));

	itemlist = itemlist_new_from_blist(_buddy_is_xmpp);
	request = request_new_from_itemlist(itemlist);

	tmpstring = g_strdup_printf(
			_("Suggest a selection of buddies to contact %s <%s>:"),
			purple_buddy_get_alias(b), purple_buddy_get_name(b));

	purple_request_fields(rosterx_plugin,
			purple_account_get_username(purple_buddy_get_account(b)),
			_("Select Buddy"),
			tmpstring,
			request,
			_("_Send"), G_CALLBACK(select_contacts_ok),
			_("_Cancel"), G_CALLBACK(select_contacts_cancel),
			NULL, NULL, NULL,
			aux);

	g_free(tmpstring);
	itemlist_destroy(itemlist);
}

/*
 * RosterX / XEP-0144 -specfic part of iq / message handling
 */
static gboolean
rosterx_process_common(PurpleConnection *pc, const char *type, const char *id,
		const char *from, xmlnode *xnode, const char *text)
{
	GList *itemlist;
	AuxData *aux;
	
	g_return_val_if_fail(xnode, FALSE);

   	aux = auxdata_new(pc);
	aux->target_jid = create_bare_jid(from);
	itemlist = itemlist_new_from_xnode(xnode);
	itemlist = itemlist_filter(itemlist, _item_is_not_in_roster, aux);

	searchresults_new_from_itemlist(itemlist, aux);

	itemlist_destroy(itemlist);
	auxdata_destroy(aux);
	return TRUE;
}

static gboolean
rosterx_process_iq(PurpleConnection *pc, const char *type, const char *id,
		const char *from, xmlnode *xnode)
{
	gboolean iq_is_ok = FALSE;
	PurpleBuddy *b = purple_find_buddy(purple_connection_get_account(pc), from);
	xmlnode *reply = xmlnode_new("iq");

	xmlnode_set_attrib(reply, "to", from);
	xmlnode_set_attrib(reply, "id", id);

	if (equals("set", type)) {
		gboolean is_subscribed = jid_is_subscribed(pc, from);

		if (b && is_subscribed) {
			iq_is_ok = TRUE;
			xmlnode_set_attrib(reply, "type", "result");

		} else { /* For error types, see XEP-0144, Section 5.1 */
			// TODO: combine error handling for iqs and messges
			xmlnode *error, *errortype;

			xmlnode_set_attrib(reply, "type", "error");

			error = xmlnode_new_child(reply, "error");
			xmlnode_set_attrib(error, "type", "auth");

			if (!b) { /* sending entity is not in roster */
				errortype = xmlnode_new_child(error, "not-authorized");
			} else {  /* not subscribed */
				errortype = xmlnode_new_child(error, "registration-required");
			}
			xmlnode_set_namespace(errortype, NS_XMPP_STANZAS);
		}
	}
	else if (equals("result", type)) {
		// TODO: unregister iq tag in hash storage, or return FALSE
		return TRUE;
	}
	else if (equals("error", type)) {
		// TODO: unregister iq tag in hash storage, or return FALSE

		// error processing
		// TODO: show error message?
		return TRUE;
	}
	else { /* e.g. type == 'get' */
		xmlnode *error, *errortype;

		xmlnode_set_attrib(reply, "type", "error");

		error = xmlnode_new_child(reply, "error");
		xmlnode_set_attrib(error, "type", "modify");

		errortype = xmlnode_new_child(error, "bad-request");
		xmlnode_set_namespace(errortype, NS_XMPP_STANZAS);
	}

	purple_signal_emit(purple_connection_get_prpl(pc),
			"jabber-sending-xmlnode", pc, &reply);
	xmlnode_free(reply);

	if (iq_is_ok)
		return rosterx_process_common(pc, type, id, from, xnode, NULL);
	else
		return TRUE;
}

static gboolean
rosterx_process_message(PurpleConnection *pc, const char *type, const char *id,
		const char *from, xmlnode *xnode, const char *text)
{
	PurpleBuddy *b = purple_find_buddy(purple_connection_get_account(pc), from);
	gboolean is_subscribed = jid_is_subscribed(pc, from);

	if (equals("error", type)) {
		// error processing
		return TRUE;

	} else {
		if (!b || !is_subscribed) {
			purple_debug_warning(PLUGIN_ID, "process_message(): Message from unsubscribed or unknown entity %s, ignoring!\n", from);
			return TRUE; /* consume message */
		}
		return rosterx_process_common(pc, type, id, from, xnode, text);
	}
}


/*
 * Generic iq / message handling, namespace disambiguation
 */
static gboolean
iq_received_cb(PurpleConnection *pc, const char *type, const char *id,
		const char *from, xmlnode *iq)
{
	xmlnode *xnode;
	const char *ns;

	xnode = xmlnode_get_child(iq, "x");
	if (!xnode)
		return FALSE;

	ns = xmlnode_get_namespace(xnode);
	purple_debug_info(PLUGIN_ID, "iq_received_cb(): from=%s, namespace=%s\n", from, ns);

	if (equals(NS_ROSTERX, ns))
		return rosterx_process_iq(pc, type, id, from, xnode);

	return FALSE;
}

static gboolean
message_received_cb(PurpleConnection *pc, const char *type, const char *id,
		const char *from, const char *to, xmlnode *message)
{
	xmlnode *xnode, *body;
	const char *ns;
	char *text = NULL;

	xnode = xmlnode_get_child(message, "x");
	if (!xnode)
		return FALSE;

	body = xmlnode_get_child(message, "body");
	if (body)
		text = xmlnode_get_data(body);

	ns = xmlnode_get_namespace(xnode);
	purple_debug_info(PLUGIN_ID, "message_received_cb(): from=%s, namespace=%s\n", from, ns);

	if (equals(NS_ROSTERX, ns))
		return rosterx_process_message(pc, type, id, from, xnode, text);

	g_free(text);
	return FALSE;
}


static void
blist_node_extended_menu_cb(PurpleBlistNode *node, GList **menu, gpointer plugin)
{
	if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
		PurpleBuddy *b = (PurpleBuddy *) node;
		PurpleAccount *account = purple_buddy_get_account(b);
		PurpleConnection *pc = purple_account_get_connection(account);

		const char *jid = purple_buddy_get_name(b);
		const char *protocol_id = purple_account_get_protocol_id(account);
		gboolean option_is_available = jid_is_subscribed(pc, jid);

		if (equals("prpl-jabber", protocol_id)) {
			PurpleMenuAction *action = purple_menu_action_new(
					_("Send contact suggestion"),
					option_is_available ? PURPLE_CALLBACK(select_contacts) : NULL,
					plugin, NULL);

			(*menu) = g_list_prepend(*menu, action);
		}
	}
}

static gboolean
add_feature_rosterx()
{
	/* Being careful to use the add_feature IPC call only once,
	 * because we do not have a remove_feature call to undo it.
	 */
	static gboolean feature_is_registered = FALSE;

	void *jabber_handle = purple_plugins_find_with_id("prpl-jabber");
	gboolean ipc_success;

	if (!feature_is_registered) {
		purple_plugin_ipc_call(jabber_handle, "add_feature",
				&ipc_success, NS_ROSTERX);
		g_return_val_if_fail(ipc_success, FALSE);

		feature_is_registered = TRUE; 
	}
	return TRUE;
}

static gboolean
plugin_load(PurplePlugin *plugin)
{
	void *jabber_handle = purple_plugins_find_with_id("prpl-jabber");
	void *blist_handle = purple_blist_get_handle();

	purple_debug_info(PLUGIN_ID, "XMPP Roster Exchange plugin loading\n");
	g_return_val_if_fail(jabber_handle, FALSE);

	if (!add_feature_rosterx())
		return FALSE;

	purple_signal_connect(jabber_handle, "jabber-receiving-iq", plugin,
			PURPLE_CALLBACK(iq_received_cb), NULL);
	purple_signal_connect(jabber_handle, "jabber-receiving-message", plugin,
			PURPLE_CALLBACK(message_received_cb), NULL);

	purple_signal_connect(blist_handle, "blist-node-extended-menu",
			plugin, PURPLE_CALLBACK(blist_node_extended_menu_cb), NULL);

	rosterx_plugin = plugin;
	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	void *jabber_handle = purple_plugins_find_with_id("prpl-jabber");

	purple_debug_info(PLUGIN_ID,
			"XMPP Roster Exchange plugin unloading\n");

	purple_signals_disconnect_by_handle(jabber_handle);

	return TRUE;
}

static PurplePluginInfo info = {
	PURPLE_PLUGIN_MAGIC,              /* magic number */
	PURPLE_MAJOR_VERSION,             /* purple major */
	10 /*PURPLE_MINOR_VERSION */,     /* purple minor: normally PURPLE_MINOR_VERSION
	                                   * Use an explicit number if you want to use the plugin also with older
	                                   * versions of Pidgin, e.g. 10 for Pidgin 2.10 and later */

	PURPLE_PLUGIN_STANDARD,           /* plugin type */
	NULL,                             /* UI requirement */
	0,                                /* flags */
	NULL,                             /* dependencies */
	PURPLE_PRIORITY_DEFAULT,          /* priority */

	PLUGIN_ID,                        /* id */
	"XMPP Roster Exchange",           /* The plugin's UI display name */
	PLUGIN_VERSION,                   /* version */
	"Allows sending contact suggestions to a buddy", /* summary */

	"The XMPP Roster Item Exchange plugin allows you to suggest other contacts "
		"from your roster to one of your buddies. "
		"This is an implementation of the client-to-client aspect of XEP-0144.", 
	                                  /* description */
	PLUGIN_AUTHOR,                    /* author */
	PLUGIN_HOMEPAGE,                  /* homepage */

	plugin_load,                      /* load */
	plugin_unload,                    /* unload */
	NULL,                             /* destroy */

	NULL,                             /* ui info */
	NULL,                             /* extra info */
	NULL,                             /* prefs info */
	NULL,                             /* actions */
	NULL,                             /* reserved */
	NULL,                             /* reserved */
	NULL,                             /* reserved */
	NULL                              /* reserved */
};

static void                        
init_plugin(PurplePlugin *plugin)
{                                  
}

PURPLE_INIT_PLUGIN(core-dzzinstant-rosterx, init_plugin, info)
