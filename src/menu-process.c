/* Tree of desktop entries */

/*
 * Copyright (C) 2002 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "menu-process.h"
#include "menu-layout.h"
#include "menu-entries.h"
#include "canonicalize.h"
#include "desktop_file.h"
#include <stdio.h>
#include <string.h>

#include <libintl.h>
#define _(x) gettext ((x))
#define N_(x) x

static void menu_node_resolve_files (MenuNode   *node);

static MenuNode*
find_menu_child (MenuNode *node)
{
  MenuNode *child;

  child = menu_node_get_children (node);
  while (child && menu_node_get_type (child) != MENU_NODE_MENU)
    child = menu_node_get_next (child);

  return child;
}

static void
merge_resolved_copy_of_children (MenuNode   *where,
                                 MenuNode   *from)
{
  MenuNode *from_child;
  MenuNode *insert_after;
  MenuNode *from_copy;
  MenuNode *menu_child;
  
  /* Copy and file-resolve the node */
  from_copy = menu_node_deep_copy (from);
  menu_node_resolve_files (from_copy);
  
  insert_after = where;

  /* skip root node */
  menu_child = find_menu_child (from_copy);
  g_assert (menu_child != NULL);

  /* merge children of toplevel <Menu> */
  from_child = menu_node_get_children (menu_child);
  while (from_child != NULL)
    {
      MenuNode *next;

      next = menu_node_get_next (from_child);
      
      switch (menu_node_get_type (from_child))
        {
        case MENU_NODE_NAME:
          menu_node_unlink (from_child); /* delete this */
          break;
          
        default:
          {
            menu_node_steal (from_child);
            menu_node_insert_after (insert_after, from_child);
            insert_after = from_child;
            menu_node_unref (from_child);
          }
          break;
        }
      
      from_child = next;
    }

  /* Now "from_copy" should be a single root node plus a single <Menu>
   * node below it, possibly with some PASSTHROUGH nodes mixed in.
   */
  g_assert (menu_node_get_type (from_copy) == MENU_NODE_ROOT);
  g_assert (menu_node_get_children (from_copy) != NULL);
  g_assert (menu_node_get_type (menu_node_get_children (from_copy)) == MENU_NODE_MENU);
  
  menu_node_unref (from_copy);
}

static void
menu_node_resolve_files (MenuNode   *node)
{
  MenuNode *child;

  /* FIXME
   * if someone does <MergeFile>A.menu</MergeFile> inside
   * A.menu, or a more elaborate loop involving multiple
   * files, we'll just get really hosed and eat all the RAM
   * we can find
   */
  
  child = menu_node_get_children (node);

  while (child != NULL)
    {
      MenuNode *next;

      /* get next first, because we delete this child (and place new
       * file contents between "child" and "next")
       */
      next = menu_node_get_next (child);
      
      switch (menu_node_get_type (child))
        {
        case MENU_NODE_MERGE_FILE:
          {
            MenuNode *to_merge;
            char *filename;

            filename = menu_node_get_content_as_path (child);
            if (filename == NULL)
              goto done;

            menu_verbose ("Merging file \"%s\"\n", filename);
            
            to_merge = menu_node_get_for_file (filename, NULL); /* missing files ignored */
            if (to_merge == NULL)
              goto done;
            
            merge_resolved_copy_of_children (child, to_merge);

            menu_node_unref (to_merge);

          done:
            g_free (filename);
            menu_node_unlink (child); /* delete this child, replaced
                                       * by the merged content
                                       */
          }
          break;
        case MENU_NODE_MERGE_DIR:
          {
            /* FIXME don't just delete it ;-) */
            
            menu_node_unlink (child);
          }
          break;
        case MENU_NODE_LEGACY_DIR:
          {
            /* FIXME don't just delete it ;-) */
            
            menu_node_unlink (child);
          }
          break;

#if 0
          /* FIXME may as well expand these here */
        case MENU_NODE_DEFAULT_APP_DIRS:
          break;
        case MENU_NODE_DEFAULT_DIRECTORY_DIRS:
          break;
#endif
          
        default:
          break;
        }

      child = next;
    }
}

static int
null_safe_strcmp (const char *a,
                  const char *b)
{
  if (a == NULL && b == NULL)
    return 0;
  else if (a == NULL)
    return -1;
  else if (b == NULL)
    return 1;
  else
    return strcmp (a, b);
}

static int
node_compare_func (const void *a,
                   const void *b)
{
  MenuNode *node_a = (MenuNode*) a;
  MenuNode *node_b = (MenuNode*) b;
  MenuNodeType t_a = menu_node_get_type (node_a);
  MenuNodeType t_b = menu_node_get_type (node_b);

  if (t_a < t_b)
    return -1;
  else if (t_a > t_b)
    return 1;
  else
    {
      const char *c_a = menu_node_get_content (node_a);
      const char *c_b = menu_node_get_content (node_b);
 
      return null_safe_strcmp (c_a, c_b);
    }
}

static int
node_menu_compare_func (const void *a,
                        const void *b)
{
  MenuNode *node_a = (MenuNode*) a;
  MenuNode *node_b = (MenuNode*) b;

  return null_safe_strcmp (menu_node_menu_get_name (node_a),
                           menu_node_menu_get_name (node_b));
}

static void
move_children (MenuNode *from,
               MenuNode *to)
{
  MenuNode *from_child;
  MenuNode *insert_before;

  insert_before = menu_node_get_children (to);
  from_child = menu_node_get_children (from);

  while (from_child != NULL)
    {
      MenuNode *next;

      next = menu_node_get_next (from_child);

      menu_node_steal (from_child);
      menu_node_insert_before (insert_before, from_child);
      
      from_child = next;
    }
}

static void
menu_node_strip_duplicate_children (MenuNode *node)
{
  GSList *simple_nodes;
  GSList *menu_nodes;
  GSList *move_nodes;
  GSList *prev;
  GSList *tmp;
  MenuNode *child;

  /* to strip dups, we find all the child nodes where
   * we want to kill dups, sort them,
   * then nuke the adjacent nodes that are equal
   */
  
  simple_nodes = NULL;
  menu_nodes = NULL;
  move_nodes = NULL;
  child = menu_node_get_children (node);
  while (child != NULL)
    {
      switch (menu_node_get_type (child))
        {
          /* These are dups if their content is the same */
        case MENU_NODE_APP_DIR:
        case MENU_NODE_DIRECTORY_DIR:
        case MENU_NODE_DIRECTORY:
          simple_nodes = g_slist_prepend (simple_nodes, child);
          break;

          /* These have to be merged in a more complicated way,
           * and then recursed
           */
        case MENU_NODE_MENU:
          menu_nodes = g_slist_prepend (menu_nodes, child);
          break;

          /* These have to be merged in a different more complicated way */
        case MENU_NODE_MOVE:
          move_nodes = g_slist_prepend (move_nodes, child);
          break;

        default:
          break;
        }

      child = menu_node_get_next (child);
    }

  /* Note that the lists are all backward. So we want to keep
   * the items that are earlier in the list, because they were
   * later in the file
   */

  /* stable sort the simple nodes */
  simple_nodes = g_slist_sort (simple_nodes,
                               node_compare_func);

  prev = NULL;
  tmp = simple_nodes;
  while (tmp != NULL)
    {
      if (prev)
        {
          MenuNode *p = prev->data;
          MenuNode *n = tmp->data;

          if (node_compare_func (p, n) == 0)
            {
              /* nuke it! */
              menu_node_unlink (n);
            }
        }
      
      prev = tmp;
      tmp = tmp->next;
    }

  g_slist_free (simple_nodes);
  simple_nodes = NULL;

  /* stable sort the menu nodes */
  menu_nodes = g_slist_sort (menu_nodes,
                             node_menu_compare_func);

  prev = NULL;
  tmp = menu_nodes;
  while (tmp != NULL)
    {
      if (prev)
        {
          MenuNode *p = prev->data;
          MenuNode *n = tmp->data;

          if (node_compare_func (p, n) == 0)
            {
              /* Move children of first menu to the start of second
               * menu and nuke the first menu
               */
              move_children (n, p);
              menu_node_unlink (n);
            }
        }
      
      prev = tmp;
      tmp = tmp->next;
    }

  g_slist_free (menu_nodes);
  menu_nodes = NULL;

  /* move nodes are pretty much annoying as hell */

  /* FIXME */
  
  g_slist_free (move_nodes);
  move_nodes = NULL;
  
  /* Finally, recursively clean up our children */
  child = menu_node_get_children (node);
  while (child != NULL)
    {
      if (menu_node_get_type (child) == MENU_NODE_MENU)
        menu_node_strip_duplicate_children (child);

      child = menu_node_get_next (child);
    }
}

typedef struct
{
  char   *name;
  Entry  *dir_entry;
  GSList *entries;
  GSList *subdirs;
} TreeNode;

struct _DesktopEntryTree
{
  char *menu_file;
  char *menu_file_dir;
  MenuNode *orig_node;
  MenuNode *resolved_node;
  TreeNode *root;
  int refcount;
};

static void build_tree     (DesktopEntryTree *tree);
static void tree_node_free (TreeNode *node);

DesktopEntryTree*
desktop_entry_tree_load (const char  *filename,
                         GError     **error)
{
  DesktopEntryTree *tree;
  MenuNode *orig_node;
  MenuNode *resolved_node;
  char *canonical;

  canonical = g_canonicalize_file_name (filename);
  if (canonical == NULL)
    {
      g_set_error (error, G_FILE_ERROR,
                   G_FILE_ERROR_FAILED,
                   _("Could not canonicalize filename \"%s\"\n"),
                   filename);
      return NULL;
    }
  
  orig_node = menu_node_get_for_canonical_file (canonical,
                                                error);
  if (orig_node == NULL)
    {
      g_free (canonical);
      return NULL;
    }

  resolved_node = menu_node_deep_copy (orig_node);
  menu_node_resolve_files (resolved_node);

  menu_node_strip_duplicate_children (resolved_node);

  tree = g_new0 (DesktopEntryTree, 1);
  tree->menu_file = canonical;
  tree->menu_file_dir = g_path_get_dirname (canonical);
  tree->orig_node = orig_node;
  tree->resolved_node = resolved_node;
  tree->root = NULL;
  tree->refcount = 1;
  
  return tree;
}

void
desktop_entry_tree_unref (DesktopEntryTree *tree)
{
  g_return_if_fail (tree != NULL);
  g_return_if_fail (tree->refcount > 0);

  tree->refcount -= 1;

  if (tree->refcount == 0)
    {
      g_free (tree->menu_file);
      g_free (tree->menu_file_dir);
      menu_node_unref (tree->orig_node);
      menu_node_unref (tree->resolved_node);
      if (tree->root)
        tree_node_free (tree->root);
      g_free (tree);
    }
}

static TreeNode*
find_subdir (TreeNode    *parent,
             const char  *subdir)
{
  GSList *tmp;

  tmp = parent->subdirs;
  while (tmp != NULL)
    {
      TreeNode *sub = tmp->data;

      if (strcmp (sub->name, subdir) == 0)
        return sub;

      tmp = tmp->next;
    }

  return NULL;
}

static TreeNode*
tree_node_find_subdir (TreeNode   *node,
                       const char *name)
{
  char **split;
  int i;
  TreeNode *iter;
  
  split = g_strsplit (name, "/", -1);

  iter = node;
  i = 0;
  while (iter != NULL && split[i] != NULL && *(split[i]) != '\0')
    {
      iter = find_subdir (iter, split[i]);

      ++i;
    }

  g_strfreev (split);
  return iter;
}

void
desktop_entry_tree_list_subdirs (DesktopEntryTree *tree,
                                 const char       *parent_dir,
                                 char           ***subdirs,
                                 int              *n_subdirs)
{
  TreeNode *dir;
  int len;
  GSList *tmp;
  int i;
  
  *subdirs = NULL;
  if (n_subdirs)
    *n_subdirs = 0;
  
  build_tree (tree);
  if (tree->root == NULL)
    return;

  dir = tree_node_find_subdir (tree->root, parent_dir);
  if (dir == NULL)
    return;

  len = g_slist_length (dir->subdirs);
  *subdirs = g_new0 (char*, len + 1);

  i = 0;
  tmp = dir->subdirs;
  while (tmp != NULL)
    {
      TreeNode *sub = tmp->data;

      (*subdirs)[i] = g_strdup (sub->name);

      ++i;
      tmp = tmp->next;
    }

  if (n_subdirs)
    *n_subdirs = len;
}

void
desktop_entry_tree_list_entries (DesktopEntryTree *tree,
                                 const char       *parent_dir,
                                 char           ***entries,
                                 int              *n_entries)
{
  TreeNode *dir;
  int len;
  int i;
  GSList *tmp;
  
  *entries = NULL;
  if (n_entries)
    *n_entries = 0;
  
  build_tree (tree);
  if (tree->root == NULL)
    return;

  dir = tree_node_find_subdir (tree->root, parent_dir);
  if (dir == NULL)
    return;

  len = g_slist_length (dir->entries);
  *entries = g_new0 (char*, len + 1);

  i = 0;
  tmp = dir->entries;
  while (tmp != NULL)
    {
      Entry *e = tmp->data;

      (*entries)[i] = g_strdup (entry_get_absolute_path (e));

      ++i;
      tmp = tmp->next;
    }

  if (n_entries)
    *n_entries = len;
}

char*
desktop_entry_tree_get_directory (DesktopEntryTree *tree,
                                  const char       *dirname)
{
  TreeNode *dir;
  
  build_tree (tree);
  if (tree->root == NULL)
    return NULL;

  dir = tree_node_find_subdir (tree->root, dirname);
  if (dir == NULL)
    return NULL;

  return g_strdup (entry_get_absolute_path (dir->dir_entry));
}

static gboolean
foreach_dir (DesktopEntryTree            *tree,
             TreeNode                    *dir,
             int                          depth,
             DesktopEntryTreeForeachFunc  func,
             void                        *user_data)
{
  GSList *tmp;

  if (! (* func) (tree,
                  TRUE,
                  depth,
                  NULL, /* FIXME */
                  entry_get_absolute_path (dir->dir_entry),
                  user_data))
    return FALSE;
  
  tmp = dir->entries;
  while (tmp != NULL)
    {
      Entry *e = tmp->data;

      if (! (* func) (tree,
                      FALSE,
                      depth,
                      NULL, /* FIXME */
                      entry_get_absolute_path (e),
                      user_data))
        return FALSE;

      tmp = tmp->next;
    }

  tmp = dir->subdirs;
  while (tmp != NULL)
    {
      TreeNode *d = tmp->data;

      if (!foreach_dir (tree, d, depth + 1, func, user_data))
        return FALSE;

      tmp = tmp->next;
    }

  return TRUE;
}

void
desktop_entry_tree_foreach (DesktopEntryTree            *tree,
                            const char                  *parent_dir,
                            DesktopEntryTreeForeachFunc  func,
                            void                        *user_data)
{
  TreeNode *dir;
  
  build_tree (tree);
  if (tree->root == NULL)
    return;
  
  dir = tree_node_find_subdir (tree->root, parent_dir);
  if (dir == NULL)
    return;

  foreach_dir (tree, dir, 0, func, user_data);
}

static TreeNode*
tree_node_new (void)
{
  TreeNode *node;

  node = g_new (TreeNode, 1);
  node->name = NULL;
  node->dir_entry = NULL;
  node->entries = NULL;
  node->subdirs = NULL;

  return node;
}

static void
tree_node_free (TreeNode *node)
{
  GSList *tmp;

  g_free (node->name);
  
  tmp = node->subdirs;
  while (tmp != NULL)
    {
      tree_node_free (tmp->data);
      tmp = tmp->next;
    }
  g_slist_free (node->subdirs);

  tmp = node->entries;
  while (tmp != NULL)
    {
      entry_unref (tmp->data);
      tmp = tmp->next;
    }
  g_slist_free (node->entries);

  if (node->dir_entry)
    entry_unref (node->dir_entry);

  g_free (node);
}

static gboolean
tree_node_free_if_broken (TreeNode *node)
{
  if (node->name == NULL ||
      node->dir_entry == NULL)
    {
      menu_verbose ("Broken node name = %p dir_entry = %p for <Menu>\n",
                    node->name, node->dir_entry);
      tree_node_free (node);
      return TRUE;
    }
  else
    return FALSE;
}

static EntrySet*
menu_node_to_entry_set (EntryDirectoryList *list,
                        MenuNode           *node)
{
  EntrySet *set;

  set = NULL;
  
  switch (menu_node_get_type (node))
    {
    case MENU_NODE_AND:
      {
        MenuNode *child;
        
        child = menu_node_get_children (node);
        while (child != NULL)
          {
            EntrySet *child_set;
            child_set = menu_node_to_entry_set (list, child);

            if (set == NULL)
              set = child_set;
            else
              {
                entry_set_intersection (set, child_set);
                entry_set_unref (child_set);
              }

            /* as soon as we get empty results, we can bail,
             * because it's an AND
             */
            if (entry_set_get_count (set) == 0)
              break;
            
            child = menu_node_get_next (child);
          }
      }
      break;
      
    case MENU_NODE_OR:
      {
        MenuNode *child;
        
        child = menu_node_get_children (node);
        while (child != NULL)
          {
            EntrySet *child_set;
            child_set = menu_node_to_entry_set (list, child);
            
            if (set == NULL)
              set = child_set;
            else
              {
                entry_set_union (set, child_set);
                entry_set_unref (child_set);
              }
            
            child = menu_node_get_next (child);
          }
      }
      break;
      
    case MENU_NODE_NOT:
      {
        /* First get the OR of all the rules */
        MenuNode *child;
        
        child = menu_node_get_children (node);
        while (child != NULL)
          {
            EntrySet *child_set;
            child_set = menu_node_to_entry_set (list, child);
            
            if (set == NULL)
              set = child_set;
            else
              {
                entry_set_union (set, child_set);
                entry_set_unref (child_set);
              }
            
            child = menu_node_get_next (child);
          }

        if (set != NULL)
          {
            /* Now invert the result */
            entry_directory_list_invert_set (list, set);
          }
      }
      break;
      
    case MENU_NODE_ALL:
      set = entry_set_new ();
      entry_directory_list_get_all_desktops (list, set);
      break;

    case MENU_NODE_FILENAME:
      {
        Entry *e;
        e = entry_directory_list_get_desktop (list,
                                              menu_node_get_content (node));
        if (e != NULL)
          {
            set = entry_set_new ();
            entry_set_add_entry (set, e);
            entry_unref (e);
          }
      }
      break;
      
    case MENU_NODE_CATEGORY:
      set = entry_set_new ();
      entry_directory_list_get_by_category (list,
                                            menu_node_get_content (node),
                                            set);
      break;

    default:
      break;
    }

  if (set == NULL)
    set = entry_set_new (); /* create an empty set */
  
  return set;
}

static void
fill_tree_node_from_menu_node (TreeNode *tree_node,
                               MenuNode *menu_node)
{
  MenuNode *child;
  EntryDirectoryList *app_dirs;
  EntryDirectoryList *dir_dirs;
  EntrySet *entries;
  
  g_return_if_fail (menu_node_get_type (menu_node) == MENU_NODE_MENU);
  
  app_dirs = menu_node_menu_get_app_entries (menu_node);
  dir_dirs = menu_node_menu_get_directory_entries (menu_node);

  entries = entry_set_new ();
  
  child = menu_node_get_children (menu_node);
  while (child != NULL)
    {
      switch (menu_node_get_type (child))
        {
        case MENU_NODE_MENU:
          /* recurse */
          {
            TreeNode *child_tree;
            child_tree = tree_node_new ();
            fill_tree_node_from_menu_node (child_tree, child);
            if (!tree_node_free_if_broken (child_tree))
              tree_node->subdirs = g_slist_prepend (tree_node->subdirs,
                                                    child_tree);
          }
          break;

        case MENU_NODE_NAME:
          {
            if (tree_node->name)
              g_free (tree_node->name); /* should not happen */
            tree_node->name = g_strdup (menu_node_get_content (child));
          }
          break;
          
        case MENU_NODE_INCLUDE:
          {
            /* The match rule children of the <Include> are
             * independent (logical OR) so we can process each one by
             * itself
             */
            MenuNode *rule;
            
            rule = menu_node_get_children (child);
            while (rule != NULL)
              {
                EntrySet *rule_set;
                rule_set = menu_node_to_entry_set (app_dirs, rule);

                if (rule_set != NULL)
                  {
                    entry_set_union (entries, rule_set);
                    entry_set_unref (rule_set);
                  }
                
                rule = menu_node_get_next (rule);
              }
          }
          break;

        case MENU_NODE_EXCLUDE:
          {
            /* The match rule children of the <Exclude> are
             * independent (logical OR) so we can process each one by
             * itself
             */
            MenuNode *rule;
            
            rule = menu_node_get_children (child);
            while (rule != NULL)
              {
                EntrySet *rule_set;
                rule_set = menu_node_to_entry_set (app_dirs, rule);
                
                if (rule_set != NULL)
                  {
                    entry_set_subtract (entries, rule_set);
                    entry_set_unref (rule_set);
                  }
                
                rule = menu_node_get_next (rule);
              }
          }
          break;

        case MENU_NODE_DIRECTORY:
          {
            Entry *e;

            /* The last <Directory> to exist wins, so we always try overwriting */
            e = entry_directory_list_get_directory (dir_dirs,
                                                    menu_node_get_content (child));

            if (e != NULL)
              {
                if (tree_node->dir_entry)
                  entry_unref (tree_node->dir_entry);
                tree_node->dir_entry = e; /* pass ref ownership */
              }
          }
          break;
        default:
          break;
        }

      child = menu_node_get_next (child);
    }

  tree_node->entries = entry_set_list_entries (entries);
  entry_set_unref (entries);
}

static void
build_tree (DesktopEntryTree *tree)
{
  if (tree->root != NULL)
    return;

  tree->root = tree_node_new ();
  fill_tree_node_from_menu_node (tree->root,
                                 find_menu_child (tree->resolved_node));
  if (tree_node_free_if_broken (tree->root))
    {
      tree->root = NULL;
      menu_verbose ("Broken root node!\n");
    }
}

typedef struct
{
  unsigned int flags;

} PrintData;

static gboolean
foreach_print (DesktopEntryTree *tree,
               gboolean          is_dir,
               int               depth,
               const char       *menu_path,
               const char       *filesystem_path_to_entry,
               void             *data)
{
#define MAX_FIELDS 3
  PrintData *pd = data;
  int i;
  GnomeDesktopFile *df;
  GError *error;
  char *fields[MAX_FIELDS] = { NULL, NULL, NULL };
  char *s;
  
  pd = data;

  error = NULL;
  df = gnome_desktop_file_load (filesystem_path_to_entry, &error);
  if (df == NULL)
    {
      g_printerr ("Warning: failed to load desktop file \"%s\": %s\n",
                  filesystem_path_to_entry, error->message);
      g_error_free (error);
      return TRUE;
    }
  g_assert (error == NULL);
  
  i = depth;
  while (i > 0)
    {
      fputc (' ', stdout);
      --i;
    }

  i = 0;
  if (pd->flags & DESKTOP_ENTRY_TREE_PRINT_NAME)
    {
      if (!gnome_desktop_file_get_locale_string (df,
                                                 NULL,
                                                 "Name",
                                                 &s))
        s = g_strdup (_("<missing Name>"));

      fields[i] = s;      
      ++i;
    }
  
  if (pd->flags & DESKTOP_ENTRY_TREE_PRINT_GENERIC_NAME)
    {
      if (!gnome_desktop_file_get_locale_string (df,
                                                 NULL,
                                                 "GenericName",
                                                 &s))
        s = g_strdup (_("<missing GenericName>"));
      
      fields[i] = s;
      ++i;
    }

  if (pd->flags & DESKTOP_ENTRY_TREE_PRINT_COMMENT)
    {
      if (!gnome_desktop_file_get_locale_string (df,
                                                 NULL,
                                                 "Comment",
                                                 &s))
        s = g_strdup (_("<missing Comment>"));
      
      fields[i] = s;
      ++i;
    }

  switch (i)
    {
    case 3:
      g_print ("%s : %s : %s\n",
               fields[0], fields[1], fields[2]);
      break;
    case 2:
      g_print ("%s : %s\n",
               fields[0], fields[1]);
      break;
    case 1:
      g_print ("%s\n",
               fields[0]);
      break;
    }

  --i;
  while (i >= 0)
    {
      g_free (fields[i]);
      --i;
    }

  gnome_desktop_file_free (df);
  
  return TRUE;
}

void
desktop_entry_tree_print (DesktopEntryTree           *tree,
                          DesktopEntryTreePrintFlags  flags)
{
  PrintData pd;

  pd.flags = flags;
  
  desktop_entry_tree_foreach (tree, "/", foreach_print, &pd);
}

void
desktop_entry_tree_write_symlink_dir (DesktopEntryTree *tree,
                                      const char       *dirname)
{


}

void
desktop_entry_tree_dump_desktop_list (DesktopEntryTree *tree)
{


}

static char *only_show_in_desktop = NULL;
void
menu_set_only_show_in_desktop (const char *desktop_name)
{
  g_free (only_show_in_desktop);
  only_show_in_desktop = g_strdup (desktop_name);
  entry_set_only_show_in_name (desktop_name);
}

void
menu_set_verbose_queries (gboolean setting)
{
  /* FIXME */
}
