/*
 * Jabber-specific stubs, condensed from:
 *   libpurple/protocols/jabber/jabber.h
 *   libpurple/protocols/jabber/buddy.h
 *
 * WARNING:
 * These data structures are incomplete and even 
 * partially incorrect.
 *
 * They are only used to determine the byte offset
 * of some of the structs' elements, and should not
 * be used in any other way!
 *
 */

typedef struct _JabberStream DummyJabberStream;
typedef struct _JabberBuddy DummyJabberBuddy;

struct _JabberBuddy {
	/**
	 * A sorted list of resources in priority descending order.
	 * This means that the first resource in the list is the
	 * "most available" (see resource_compare_cb in buddy.c for
	 * details).  Don't play with this yourself, let
	 * jabber_buddy_track_resource and jabber_buddy_remove_resource do it.
	 */
	GList *resources;     /* needed in find_resources() */
	char *error_msg;
	enum {
		JABBER_INVISIBLE_NONE   = 0,
		JABBER_INVISIBLE_SERVER = 1 << 1,
		JABBER_INVIS_BUDDY      = 1 << 2
	} invisible;
	enum {
		JABBER_SUB_NONE    = 0,
		JABBER_SUB_PENDING = 1 << 1,
		JABBER_SUB_TO      = 1 << 2,
		JABBER_SUB_FROM    = 1 << 3,
		JABBER_SUB_BOTH    = (JABBER_SUB_TO | JABBER_SUB_FROM),
		JABBER_SUB_REMOVE  = 1 << 4
	} subscription;       /* needed in jid_is_subscribed() */
};


typedef struct _JabberBuddyResource {
	DummyJabberBuddy *jb;
	char *name;           /* needed in find_resources() */
	int priority;         /* maybe needed later */
	enum {DUMMY_0}/* JabberBuddyState */  state;

	char NOTE_This_struct_is_only_a_stub_of_JabberBuddyResource[0];
} DummyJabberBuddyResource;

struct _JabberStream
{
	int fd;
	gpointer /* PurpleSrvTxtQueryData * */ srv_query_data;
	gpointer /*xmlParserCtxt * */ context;
	xmlnode *current;

	struct {
		guint8 major;
		guint8 minor;
	} protocol_version;

	gpointer /*JabberSaslMech * */ auth_mech;
	gpointer auth_mech_data;

	char *stream_id;
	enum {DUMMY_1} /* JabberStreamState */  state;

	GHashTable *buddies;   /* needed in jid_is_subscribed(), find_resources() */

	char NOTE_This_struct_is_only_a_stub_of_JabberStream[0];
};
