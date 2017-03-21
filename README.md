# Roster Item Exchange plugin for Pidgin
## Description
This plugin enables sending and receiving of contact suggestions, as described in [XEP-0144](https://xmpp.org/extensions/xep-0144.html).
As it does not depend on Gtk-specific code, it should also work with other libpurple-based clients.

## Installation (on Linux systems, procedure for MS Windows and other systems may differ)

1. Download and unpack Pidgin source, as described in [Installing Pidgin](https://developer.pidgin.im/wiki/Installing%20Pidgin).
2. Change to the source's main directory and run `./configure` (or `./autogen.sh` if you obtained the source from the hg repository).  
  You don't have to run make if you only want to build the plugin.
3. Copy the plugin source code to the libpurple plugin directory:  
  ```
  cd <the directory which contains the downloaded source code of the plugin>
  cp xmpp-rosterx.[ch] <directory with the unpacked pidgin source>/libpurple/plugins/
  ```
  - For compatibility with prior versions of Pidgin (like the stock version that comes with your distribution), you may lower the plugin's version number (at your own risk), e.g. if you want to use the plugin with Pidgin 2.10.6, just replace `PURPLE_MINOR_VERSION` with `10` in the plugin's `PurplePluginInfo` struct.
4. Compile the plugin:  
  ```
  cd <directory with the unpacked pidgin source>/libpurple/plugins/
  make xmpp-roster.so
  ```
5. Copy compiled plugin to your home directory (you may have to create the `plugins` subdirectory):  
  ```
  mkdir ~/.purple/plugins                  # If it doesn't exist yet
  cp xmpp-rosterx.so ~/.purple/plugins/
  ```
6. (Re-)start Pidgin, go to the **Tools > Plugins** menu item, activate **XMPP Roster Exchange** plugin.

The function `Send contact suggestion` can now be used...
- from the Buddy List: in the context menu of each Jabber contact
- from a conversation window: in the submenu **Conversation > More**
