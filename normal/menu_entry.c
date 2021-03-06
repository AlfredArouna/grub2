/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2005,2006,2007,2008,2009  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/normal.h>
#include <grub/term.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/loader.h>
#include <grub/command.h>
#include <grub/parser.h>
#include <grub/auth.h>
#include <grub/i18n.h>

enum update_mode
  {
    NO_LINE,
    SINGLE_LINE,
    ALL_LINES
  };

struct line
{
  /* The line buffer.  */
  char *buf;
  /* The length of the line.  */
  int len;
  /* The maximum length of the line.  */
  int max_len;
};

struct per_term_screen
{
  struct grub_term_output *term;
  /* The X coordinate.  */
  int x;
  /* The Y coordinate.  */
  int y;
};

struct screen
{
  /* The array of lines.  */
  struct line *lines;
  /* The number of lines.  */
  int num_lines;
  /* The current column.  */
  int column;
  /* The real column.  */
  int real_column;
  /* The current line.  */
  int line;
  /* The kill buffer.  */
  char *killed_text;
  /* The flag of a completion window.  */
  int completion_shown;

  struct per_term_screen *terms;
  unsigned nterms;
};

/* Used for storing completion items temporarily.  */
static struct line completion_buffer;
static int completion_type;

/* Initialize a line.  */
static int
init_line (struct line *linep)
{
  linep->len = 0;
  linep->max_len = 80; /* XXX */
  linep->buf = grub_malloc (linep->max_len);
  if (! linep->buf)
    return 0;

  return 1;
}

/* Allocate extra space if necessary.  */
static int
ensure_space (struct line *linep, int extra)
{
  if (linep->max_len < linep->len + extra)
    {
      linep->max_len = linep->len + extra + 80; /* XXX */
      linep->buf = grub_realloc (linep->buf, linep->max_len + 1);
      if (! linep->buf)
	return 0;
    }

  return 1;
}

/* Return the number of lines occupied by this line on the screen.  */
static int
get_logical_num_lines (struct line *linep, struct per_term_screen *term_screen)
{
  return (linep->len / grub_term_entry_width (term_screen->term)) + 1;
}

/* Print a line.  */
static void
print_line (struct line *linep, int offset, int start, int y,
	    struct per_term_screen *term_screen)
{
  grub_term_gotoxy (term_screen->term, 
		    GRUB_TERM_LEFT_BORDER_X + GRUB_TERM_MARGIN + start + 1,
		    y + GRUB_TERM_FIRST_ENTRY_Y);

  if (linep->len >= offset + grub_term_entry_width (term_screen->term))
    {
      char *p, c;
      p = linep->buf + offset + grub_term_entry_width (term_screen->term);
      c = *p;
      *p = 0;
      grub_puts_terminal (linep->buf + offset + start, term_screen->term);
      *p = c;
      grub_putcode ('\\', term_screen->term);
    }
  else
    {
      int i;
      char *p, c;

      p = linep->buf + linep->len;
      c = *p;
      *p = 0;
      grub_puts_terminal (linep->buf + offset + start, term_screen->term);
      *p = c;

      for (i = 0;
	   i <= grub_term_entry_width (term_screen->term) - linep->len + offset;
	   i++)
	grub_putcode (' ', term_screen->term);
    }
}

/* Print an empty line.  */
static void
print_empty_line (int y, struct per_term_screen *term_screen)
{
  int i;

  grub_term_gotoxy (term_screen->term,
		    GRUB_TERM_LEFT_BORDER_X + GRUB_TERM_MARGIN + 1,
		    y + GRUB_TERM_FIRST_ENTRY_Y);

  for (i = 0; i < grub_term_entry_width (term_screen->term) + 1; i++)
    grub_putcode (' ', term_screen->term);
}

/* Print an up arrow.  */
static void
print_up (int flag, struct per_term_screen *term_screen)
{
  grub_term_gotoxy (term_screen->term, GRUB_TERM_LEFT_BORDER_X 
		    + grub_term_entry_width (term_screen->term),
		    GRUB_TERM_FIRST_ENTRY_Y);

  if (flag)
    grub_putcode (GRUB_TERM_DISP_UP, term_screen->term);
  else
    grub_putcode (' ', term_screen->term);
}

/* Print a down arrow.  */
static void
print_down (int flag, struct per_term_screen *term_screen)
{
  grub_term_gotoxy (term_screen->term, GRUB_TERM_LEFT_BORDER_X
		    + grub_term_border_width (term_screen->term),
		    GRUB_TERM_TOP_BORDER_Y 
		    + grub_term_num_entries (term_screen->term));

  if (flag)
    grub_putcode (GRUB_TERM_DISP_DOWN, term_screen->term);
  else
    grub_putcode (' ', term_screen->term);
}

/* Draw the lines of the screen SCREEN.  */
static void
update_screen (struct screen *screen, struct per_term_screen *term_screen,
	       int region_start, int region_column,
	       int up, int down, enum update_mode mode)
{
  int up_flag = 0;
  int down_flag = 0;
  int y;
  int i;
  struct line *linep;

  /* Check if scrolling is necessary.  */
  if (term_screen->y < 0 || term_screen->y
      >= grub_term_num_entries (term_screen->term))
    {
      if (term_screen->y < 0)
	term_screen->y = 0;
      else
	term_screen->y = grub_term_num_entries (term_screen->term) - 1;

      region_start = 0;
      region_column = 0;
      up = 1;
      down = 1;
      mode = ALL_LINES;
    }

  if (mode != NO_LINE)
    {
      /* Draw lines. This code is tricky, because this must calculate logical
	 positions.  */
      y = term_screen->y - screen->column
	/ grub_term_entry_width (term_screen->term);
      i = screen->line;
      linep = screen->lines + i;
      while (y > 0)
	{
	   i--;
	   linep--;
	   y -= get_logical_num_lines (linep, term_screen);
	}

      if (y < 0 || i > 0)
	up_flag = 1;

      do
	{
	  int column;

	  for (column = 0;
	       column <= linep->len
		 && y < grub_term_num_entries (term_screen->term);
	       column += grub_term_entry_width (term_screen->term), y++)
	    {
	      if (y < 0)
		continue;

	      if (i == region_start)
		{
		  if (region_column >= column
		      && region_column
		      < (column
			 + grub_term_entry_width (term_screen->term)))
		    print_line (linep, column, region_column - column, y,
				term_screen);
		  else if (region_column < column)
		    print_line (linep, column, 0, y, term_screen);
		}
	      else if (i > region_start && mode == ALL_LINES)
		print_line (linep, column, 0, y, term_screen);
	    }

	  if (y == grub_term_num_entries (term_screen->term))
	    {
	      if (column <= linep->len || i + 1 < screen->num_lines)
		down_flag = 1;
	    }

	  linep++;
	  i++;

	  if (mode == ALL_LINES && i == screen->num_lines)
	    for (; y < grub_term_num_entries (term_screen->term); y++)
	      print_empty_line (y, term_screen);

	}
      while (y < grub_term_num_entries (term_screen->term));

      /* Draw up and down arrows.  */
      if (up)
	print_up (up_flag, term_screen);
      if (down)
	print_down (down_flag, term_screen);
    }

  /* Place the cursor.  */
  grub_term_gotoxy (term_screen->term, 
		    GRUB_TERM_LEFT_BORDER_X + GRUB_TERM_MARGIN + 1
		    + term_screen->x,
		    GRUB_TERM_FIRST_ENTRY_Y + term_screen->y);

  grub_term_refresh (term_screen->term);
}

static void
update_screen_all (struct screen *screen,
		   int region_start, int region_column,
		   int up, int down, enum update_mode mode)
{
  unsigned i;
  for (i = 0; i < screen->nterms; i++)
    update_screen (screen, &screen->terms[i], region_start, region_column,
		   up, down, mode);
}

static int
insert_string (struct screen *screen, char *s, int update)
{
  int region_start = screen->num_lines;
  int region_column = 0;
  int down[screen->nterms];
  enum update_mode mode[screen->nterms];
  unsigned i;

  for (i = 0; i < screen->nterms; i++)
    {
      down[i] = 0;
      mode[i] = NO_LINE;
    }

  while (*s)
    {
      if (*s == '\n')
	{
	  /* LF is special because it creates a new line.  */
	  struct line *current_linep;
	  struct line *next_linep;
	  int size;

	  /* Make a new line.  */
	  screen->num_lines++;
	  screen->lines = grub_realloc (screen->lines,
					screen->num_lines
					* sizeof (struct line));
	  if (! screen->lines)
	    return 0;

	  /* Scroll down. */
	  grub_memmove (screen->lines + screen->line + 2,
			screen->lines + screen->line + 1,
			((screen->num_lines - screen->line - 2)
			 * sizeof (struct line)));

	  if (! init_line (screen->lines + screen->line + 1))
	    return 0;

	  /* Fold the line.  */
	  current_linep = screen->lines + screen->line;
	  next_linep = current_linep + 1;
	  size = current_linep->len - screen->column;

	  if (! ensure_space (next_linep, size))
	    return 0;

	  grub_memmove (next_linep->buf,
			current_linep->buf + screen->column,
			size);
	  current_linep->len = screen->column;
	  next_linep->len = size;

	  /* Update a dirty region.  */
	  if (region_start > screen->line)
	    {
	      region_start = screen->line;
	      region_column = screen->column;
	    }

	  for (i = 0; i < screen->nterms; i++)
	    {
	      mode[i] = ALL_LINES;
	      down[i] = 1; /* XXX not optimal.  */
	    }

	  /* Move the cursor.  */
	  screen->column = screen->real_column = 0;
	  screen->line++;
	  for (i = 0; i < screen->nterms; i++)
	    {
	      screen->terms[i].x = 0;
	      screen->terms[i].y++;
	    }
	  s++;
	}
      else
	{
	  /* All but LF.  */
	  char *p;
	  struct line *current_linep;
	  int size;
	  int orig_num[screen->nterms], new_num[screen->nterms];

	  /* Find a string delimited by LF.  */
	  p = grub_strchr (s, '\n');
	  if (! p)
	    p = s + grub_strlen (s);

	  /* Insert the string.  */
	  current_linep = screen->lines + screen->line;
	  size = p - s;
	  if (! ensure_space (current_linep, size))
	    return 0;

	  grub_memmove (current_linep->buf + screen->column + size,
			current_linep->buf + screen->column,
			current_linep->len - screen->column);
	  grub_memmove (current_linep->buf + screen->column,
			s,
			size);
	  for (i = 0; i < screen->nterms; i++)
	    orig_num[i] = get_logical_num_lines (current_linep,
						 &screen->terms[i]);
	  current_linep->len += size;
	  for (i = 0; i < screen->nterms; i++)
	    new_num[i] = get_logical_num_lines (current_linep,
						&screen->terms[i]);

	  /* Update the dirty region.  */
	  if (region_start > screen->line)
	    {
	      region_start = screen->line;
	      region_column = screen->column;
	    }

	  for (i = 0; i < screen->nterms; i++)
	    if (orig_num[i] != new_num[i])
	      {
		mode[i] = ALL_LINES;
		down[i] = 1; /* XXX not optimal.  */
	      }
	    else if (mode[i] != ALL_LINES)
	      mode[i] = SINGLE_LINE;

	  /* Move the cursor.  */
	  screen->column += size;
	  screen->real_column = screen->column;
	  for (i = 0; i < screen->nterms; i++)
	    {
	      screen->terms[i].x += size;
	      screen->terms[i].y += screen->terms[i].x
		/ grub_term_entry_width (screen->terms[i].term);
	      screen->terms[i].x
		%= grub_term_entry_width (screen->terms[i].term);
	    }
	  s = p;
	}
    }

  if (update)
    for (i = 0; i < screen->nterms; i++)
      update_screen (screen, &screen->terms[i],
		     region_start, region_column, 0, down[i], mode[i]);

  return 1;
}

/* Release the resource allocated for SCREEN.  */
static void
destroy_screen (struct screen *screen)
{
  int i;

  if (screen->lines)
    for (i = 0; i < screen->num_lines; i++)
      {
	struct line *linep = screen->lines + i;

	if (linep)
	  grub_free (linep->buf);
      }

  grub_free (screen->killed_text);
  grub_free (screen->lines);
  grub_free (screen->terms);
  grub_free (screen);
}

/* Make a new screen.  */
static struct screen *
make_screen (grub_menu_entry_t entry)
{
  struct screen *screen;
  unsigned i;

  /* Initialize the screen.  */
  screen = grub_zalloc (sizeof (*screen));
  if (! screen)
    return 0;

  screen->num_lines = 1;
  screen->lines = grub_malloc (sizeof (struct line));
  if (! screen->lines)
    goto fail;

  /* Initialize the first line which must be always present.  */
  if (! init_line (screen->lines))
    goto fail;

  insert_string (screen, (char *) entry->sourcecode, 0);

  /* Reset the cursor position.  */
  screen->column = 0;
  screen->real_column = 0;
  screen->line = 0;
  for (i = 0; i < screen->nterms; i++)
    {
      screen->terms[i].x = 0;
      screen->terms[i].y = 0;
    }

  return screen;

 fail:
  destroy_screen (screen);
  return 0;
}

static int
forward_char (struct screen *screen, int update)
{
  struct line *linep;
  unsigned i;

  linep = screen->lines + screen->line;
  if (screen->column < linep->len)
    {
      screen->column++;
      for (i = 0; i < screen->nterms; i++)
	{
	  screen->terms[i].x++;
	  if (screen->terms[i].x
	      == grub_term_entry_width (screen->terms[i].term))
	    {
	      screen->terms[i].x = 0;
	      screen->terms[i].y++;
	    }
	}
    }
  else if (screen->num_lines > screen->line + 1)
    {
      screen->column = 0;
      screen->line++;
      for (i = 0; i < screen->nterms; i++)
	{
	  screen->terms[i].x = 0;
	  screen->terms[i].y++;
	}
    }

  screen->real_column = screen->column;

  if (update)
    update_screen_all (screen, screen->num_lines, 0, 0, 0, NO_LINE);
  return 1;
}

static int
backward_char (struct screen *screen, int update)
{
  unsigned i;

  if (screen->column > 0)
    {
      screen->column--;
      for (i = 0; i < screen->nterms; i++)
	{
	  screen->terms[i].x--;
	  if (screen->terms[i].x == -1)
	    {
	      screen->terms[i].x
		= grub_term_entry_width (screen->terms[i].term) - 1;
	      screen->terms[i].y--;
	    }
	}
    }
  else if (screen->line > 0)
    {
      struct line *linep;

      screen->line--;
      linep = screen->lines + screen->line;
      screen->column = linep->len;
      for (i = 0; i < screen->nterms; i++)
	{
	  screen->terms[i].x = screen->column
	    % grub_term_entry_width (screen->terms[i].term);
	  screen->terms[i].y--;
	}
    }

  screen->real_column = screen->column;

  if (update)
    update_screen_all (screen, screen->num_lines, 0, 0, 0, NO_LINE);

  return 1;
}

static int
previous_line (struct screen *screen, int update)
{
  unsigned i;

  if (screen->line > 0)
    {
      struct line *linep;
      int col;

      /* How many physical lines from the current position
	 to the first physical line?  */
      col = screen->column;

      screen->line--;

      linep = screen->lines + screen->line;
      if (linep->len < screen->real_column)
	screen->column = linep->len;
      else
	screen->column = screen->real_column;

      for (i = 0; i < screen->nterms; i++)
	{
	  int dy;
	  dy = col / grub_term_entry_width (screen->terms[i].term);

	  /* How many physical lines from the current position
	     to the last physical line?  */
	  dy += (linep->len / grub_term_entry_width (screen->terms[i].term)
		 - screen->column
		 / grub_term_entry_width (screen->terms[i].term));
	
	  screen->terms[i].y -= dy + 1;
	  screen->terms[i].x
	    = screen->column % grub_term_entry_width (screen->terms[i].term);
      }
    }
  else
    {
      for (i = 0; i < screen->nterms; i++)
	{
	  screen->terms[i].y
	    -= screen->column / grub_term_entry_width (screen->terms[i].term);
	  screen->terms[i].x = 0;
	}
      screen->column = 0;
    }

  if (update)
    update_screen_all (screen, screen->num_lines, 0, 0, 0, NO_LINE);

  return 1;
}

static int
next_line (struct screen *screen, int update)
{
  unsigned i;

  if (screen->line < screen->num_lines - 1)
    {
      struct line *linep;
      int l1, c1;

      /* How many physical lines from the current position
	 to the last physical line?  */
      linep = screen->lines + screen->line;
      l1 = linep->len;
      c1 = screen->column;

      screen->line++;

      linep++;
      if (linep->len < screen->real_column)
	screen->column = linep->len;
      else
	screen->column = screen->real_column;

      for (i = 0; i < screen->nterms; i++)
	{
	  int dy;
	  dy = l1 / grub_term_entry_width (screen->terms[i].term)
	    - c1 / grub_term_entry_width (screen->terms[i].term);
	  /* How many physical lines from the current position
	     to the first physical line?  */
	  dy += screen->column / grub_term_entry_width (screen->terms[i].term);
	  screen->terms[i].y += dy + 1;
	  screen->terms[i].x = screen->column
	    % grub_term_entry_width (screen->terms[i].term);
	}
    }
  else
    {
      struct line *linep;
      int l, s;
      
      linep = screen->lines + screen->line;
      l = linep->len;
      s = screen->column;
      screen->column = linep->len;
      for (i = 0; i < screen->nterms; i++)
	{
	  screen->terms[i].y 
	    += (l / grub_term_entry_width (screen->terms[i].term)
		-  s / grub_term_entry_width (screen->terms[i].term));
	  screen->terms[i].x
	    = screen->column % grub_term_entry_width (screen->terms[i].term);
	}
    }

  if (update)
    update_screen_all (screen, screen->num_lines, 0, 0, 0, NO_LINE);

  return 1;
}

static int
beginning_of_line (struct screen *screen, int update)
{
  unsigned i;
  int col;
  
  col = screen->column;
  screen->column = screen->real_column = 0;
  for (i = 0; i < screen->nterms; i++)
    {
      screen->terms[i].x = 0;
      screen->terms[i].y -= col / grub_term_entry_width (screen->terms[i].term);
    }

  if (update)
    update_screen_all (screen, screen->num_lines, 0, 0, 0, NO_LINE);

  return 1;
}

static int
end_of_line (struct screen *screen, int update)
{
  struct line *linep;
  unsigned i;
  int col;

  linep = screen->lines + screen->line;
  col = screen->column;
  screen->column = screen->real_column = linep->len;
  for (i = 0; i < screen->nterms; i++)
    {
      screen->terms[i].y 
	+= (linep->len / grub_term_entry_width (screen->terms->term)
	    - col / grub_term_entry_width (screen->terms->term));
      screen->terms[i].x
	= screen->column % grub_term_entry_width (screen->terms->term);
    }

  if (update)
    update_screen_all (screen, screen->num_lines, 0, 0, 0, NO_LINE);

  return 1;
}

static int
delete_char (struct screen *screen, int update)
{
  struct line *linep;
  int start = screen->num_lines;
  int column = 0;

  linep = screen->lines + screen->line;
  if (linep->len > screen->column)
    {
      int orig_num[screen->nterms], new_num;
      unsigned i;

      for (i = 0; i < screen->nterms; i++)
	orig_num[i] = get_logical_num_lines (linep, &screen->terms[i]);

      grub_memmove (linep->buf + screen->column,
		    linep->buf + screen->column + 1,
		    linep->len - screen->column - 1);
      linep->len--;

      start = screen->line;
      column = screen->column;

      screen->real_column = screen->column;

      if (update)
	{
	  for (i = 0; i < screen->nterms; i++)
	    {
	      new_num = get_logical_num_lines (linep, &screen->terms[i]);
	      if (orig_num[i] != new_num)
		update_screen (screen, &screen->terms[i],
			       start, column, 0, 0, ALL_LINES);
	      else
		update_screen (screen, &screen->terms[i],
			       start, column, 0, 0, SINGLE_LINE);
	    }
	}
    }
  else if (screen->num_lines > screen->line + 1)
    {
      struct line *next_linep;

      next_linep = linep + 1;
      if (! ensure_space (linep, next_linep->len))
	return 0;

      grub_memmove (linep->buf + linep->len, next_linep->buf, next_linep->len);
      linep->len += next_linep->len;

      grub_free (next_linep->buf);
      grub_memmove (next_linep,
		    next_linep + 1,
		    (screen->num_lines - screen->line - 2)
		    * sizeof (struct line));
      screen->num_lines--;

      start = screen->line;
      column = screen->column;

      screen->real_column = screen->column;
      if (update)
	update_screen_all (screen, start, column, 0, 1, ALL_LINES);
    }

  return 1;
}

static int
backward_delete_char (struct screen *screen, int update)
{
  int saved_column;
  int saved_line;

  saved_column = screen->column;
  saved_line = screen->line;

  if (! backward_char (screen, 0))
    return 0;

  if (saved_column != screen->column || saved_line != screen->line)
    if (! delete_char (screen, update))
      return 0;

  return 1;
}

static int
kill_line (struct screen *screen, int continuous, int update)
{
  struct line *linep;
  char *p;
  int size;
  int offset;

  p = screen->killed_text;
  if (! continuous && p)
    p[0] = '\0';

  linep = screen->lines + screen->line;
  size = linep->len - screen->column;

  if (p)
    offset = grub_strlen (p);
  else
    offset = 0;

  if (size > 0)
    {
      int orig_num[screen->nterms], new_num;
      unsigned i;

      p = grub_realloc (p, offset + size + 1);
      if (! p)
	return 0;

      grub_memmove (p + offset, linep->buf + screen->column, size);
      p[offset + size - 1] = '\0';

      screen->killed_text = p;

      for (i = 0; i < screen->nterms; i++)
	orig_num[i] = get_logical_num_lines (linep, &screen->terms[i]);
      linep->len = screen->column;

      if (update)
	{
	  new_num = get_logical_num_lines (linep, &screen->terms[i]);
	  for (i = 0; i < screen->nterms; i++)
	    {
	      if (orig_num[i] != new_num)
		update_screen (screen, &screen->terms[i],
			       screen->line, screen->column, 0, 1, ALL_LINES);
	      else
		update_screen (screen, &screen->terms[i],
			       screen->line, screen->column, 0, 0, SINGLE_LINE);
	    }
	}
    }
  else if (screen->line + 1 < screen->num_lines)
    {
      p = grub_realloc (p, offset + 1 + 1);
      if (! p)
	return 0;

      p[offset] = '\n';
      p[offset + 1] = '\0';

      screen->killed_text = p;

      return delete_char (screen, update);
    }

  return 1;
}

static int
yank (struct screen *screen, int update)
{
  if (screen->killed_text)
    return insert_string (screen, screen->killed_text, update);

  return 1;
}

static int
open_line (struct screen *screen, int update)
{
  int saved_y[screen->nterms];
  unsigned i;

  for (i = 0; i < screen->nterms; i++)
    saved_y[i] = screen->terms[i].y;

  if (! insert_string (screen, "\n", 0))
    return 0;

  if (! backward_char (screen, 0))
    return 0;

  for (i = 0; i < screen->nterms; i++)
    screen->terms[i].y = saved_y[i];

  if (update)
    update_screen_all (screen, screen->line, screen->column, 0, 1, ALL_LINES);

  return 1;
}

/* A completion hook to print items.  */
static void
store_completion (const char *item, grub_completion_type_t type,
		  int count __attribute__ ((unused)))
{
  char *p;

  completion_type = type;

  /* Make sure that the completion buffer has enough room.  */
  if (completion_buffer.max_len < (completion_buffer.len
				   + (int) grub_strlen (item) + 1 + 1))
    {
      grub_size_t new_len;

      new_len = completion_buffer.len + grub_strlen (item) + 80;
      p = grub_realloc (completion_buffer.buf, new_len);
      if (! p)
	{
	  /* Possibly not fatal.  */
	  grub_errno = GRUB_ERR_NONE;
	  return;
	}
      p[completion_buffer.len] = 0;
      completion_buffer.buf = p;
      completion_buffer.max_len = new_len;
    }

  p = completion_buffer.buf + completion_buffer.len;
  if (completion_buffer.len != 0)
    {
      *p++ = ' ';
      completion_buffer.len++;
    }
  grub_strcpy (p, item);
  completion_buffer.len += grub_strlen (item);
}

static int
complete (struct screen *screen, int continuous, int update)
{
  char saved_char;
  struct line *linep;
  int restore;
  char *insert;
  static int count = -1;
  unsigned i;
  grub_uint32_t *ucs4;
  grub_size_t buflen;
  grub_ssize_t ucs4len;

  if (continuous)
    count++;
  else
    count = 0;

  completion_buffer.buf = 0;
  completion_buffer.len = 0;
  completion_buffer.max_len = 0;

  linep = screen->lines + screen->line;
  saved_char = linep->buf[screen->column];
  linep->buf[screen->column] = '\0';

  insert = grub_normal_do_completion (linep->buf, &restore, store_completion);

  linep->buf[screen->column] = saved_char;

  buflen = grub_strlen (completion_buffer.buf);
  ucs4 = grub_malloc (sizeof (grub_uint32_t) * (buflen + 1));

  if (!ucs4)
    {
      grub_print_error ();
      grub_errno = GRUB_ERR_NONE;
      return 1;
    }

  ucs4len = grub_utf8_to_ucs4 (ucs4, buflen,
			       (grub_uint8_t *) completion_buffer.buf,
			       buflen, 0);
  ucs4[ucs4len] = 0;

  if (restore)
    for (i = 0; i < screen->nterms; i++)
      {
	int num_sections = ((completion_buffer.len
			     + grub_term_width (screen->terms[i].term) - 8 - 1)
			    / (grub_term_width (screen->terms[i].term) - 8));
	grub_uint32_t *endp;
	grub_uint16_t pos;
	grub_uint32_t *p = ucs4;

	pos = grub_term_getxy (screen->terms[i].term);
	grub_term_gotoxy (screen->terms[i].term, 0,
			  grub_term_height (screen->terms[i].term) - 3);

	screen->completion_shown = 1;

	grub_term_gotoxy (screen->terms[i].term, 0,
			  grub_term_height (screen->terms[i].term) - 3);
	grub_puts_terminal ("   ", screen->terms[i].term);
	switch (completion_type)
	  {
	  case GRUB_COMPLETION_TYPE_COMMAND:
	    grub_puts_terminal (_("Possible commands are:"),
				screen->terms[i].term);
	    break;
	  case GRUB_COMPLETION_TYPE_DEVICE:
	    grub_puts_terminal (_("Possible devices are:"),
				screen->terms[i].term);
	    break;
	  case GRUB_COMPLETION_TYPE_FILE:
	    grub_puts_terminal (_("Possible files are:"),
				screen->terms[i].term);
	    break;
	  case GRUB_COMPLETION_TYPE_PARTITION:
	    grub_puts_terminal (_("Possible partitions are:"),
				screen->terms[i].term);
	    break;
	  case GRUB_COMPLETION_TYPE_ARGUMENT:
	    grub_puts_terminal (_("Possible arguments are:"),
				screen->terms[i].term);
	    break;
	  default:
	    grub_puts_terminal (_("Possible things are:"),
				screen->terms[i].term);
	    break;
	  }

	grub_puts_terminal ("\n    ", screen->terms[i].term);

	p += (count % num_sections)
	  * (grub_term_width (screen->terms[i].term) - 8);
	endp = p + (grub_term_width (screen->terms[i].term) - 8);

	if (p != ucs4)
	  grub_putcode (GRUB_TERM_DISP_LEFT, screen->terms[i].term);
	else
	  grub_putcode (' ', screen->terms[i].term);

	while (*p && p < endp)
	  grub_putcode (*p++, screen->terms[i].term);

	if (*p)
	  grub_putcode (GRUB_TERM_DISP_RIGHT, screen->terms[i].term);
	grub_term_gotoxy (screen->terms[i].term, pos >> 8, pos & 0xFF);
      }

  if (insert)
    {
      insert_string (screen, insert, update);
      count = -1;
      grub_free (insert);
    }
  else if (update)
    grub_refresh ();

  grub_free (completion_buffer.buf);
  return 1;
}

/* Clear displayed completions.  */
static void
clear_completions (struct per_term_screen *term_screen)
{
  grub_uint16_t pos;
  unsigned i, j;

  pos = grub_term_getxy (term_screen->term);
  grub_term_gotoxy (term_screen->term, 0,
		    grub_term_height (term_screen->term) - 3);

  for (i = 0; i < 2; i++)
    {
      for (j = 0; j < grub_term_width (term_screen->term) - 1; j++)
	grub_putcode (' ', term_screen->term);
      grub_putcode ('\n', term_screen->term);
    }

  grub_term_gotoxy (term_screen->term, pos >> 8, pos & 0xFF);
  grub_term_refresh (term_screen->term);
}

static void
clear_completions_all (struct screen *screen)
{
  unsigned i;

  for (i = 0; i < screen->nterms; i++)
    clear_completions (&screen->terms[i]);
}

/* Execute the command list in the screen SCREEN.  */
static int
run (struct screen *screen)
{
  int currline = 0;
  char *nextline;

  auto grub_err_t editor_getline (char **line, int cont);
  grub_err_t editor_getline (char **line, int cont __attribute__ ((unused)))
    {
      struct line *linep = screen->lines + currline;
      char *p;

      if (currline > screen->num_lines)
	{
	  *line = 0;
	  return 0;
	}

      /* Trim down space characters.  */
      for (p = linep->buf + linep->len - 1;
	   p >= linep->buf && grub_isspace (*p);
	   p--)
	;
      *++p = '\0';

      linep->len = p - linep->buf;
      for (p = linep->buf; grub_isspace (*p); p++)
	;
      *line = grub_strdup (p);
      currline++;
      return 0;
    }

  grub_cls ();
  grub_printf ("  ");
  grub_printf_ (N_("Booting a command list"));
  grub_printf ("\n\n");


  /* Execute the script, line for line.  */
  while (currline < screen->num_lines)
    {
      editor_getline (&nextline, 0);
      if (grub_parser_get_current ()->parse_line (nextline, editor_getline))
	break;
    }

  if (grub_errno == GRUB_ERR_NONE && grub_loader_is_loaded ())
    /* Implicit execution of boot, only if something is loaded.  */
    grub_command_execute ("boot", 0, 0);

  if (grub_errno != GRUB_ERR_NONE)
    {
      grub_print_error ();
      grub_errno = GRUB_ERR_NONE;
      grub_wait_after_message ();
    }

  return 1;
}

/* Edit a menu entry with an Emacs-like interface.  */
void
grub_menu_entry_run (grub_menu_entry_t entry)
{
  struct screen *screen;
  int prev_c;
  grub_err_t err = GRUB_ERR_NONE;
  unsigned i;
  grub_term_output_t term;

  err = grub_auth_check_authentication (NULL);

  if (err)
    {
      grub_print_error ();
      grub_errno = GRUB_ERR_NONE;
      return;
    }

  screen = make_screen (entry);
  if (! screen)
    return;

  screen->terms = NULL;

 refresh:
  grub_free (screen->terms);
  screen->nterms = 0;
  FOR_ACTIVE_TERM_OUTPUTS(term)
    screen->nterms++;
  screen->terms = grub_malloc (screen->nterms * sizeof (screen->terms[0]));
  if (!screen->terms)
    {
      grub_print_error ();
      grub_errno = GRUB_ERR_NONE;
      return;
    }
  i = 0;
  FOR_ACTIVE_TERM_OUTPUTS(term)
  {
    screen->terms[i].term = term;
    screen->terms[i].x = 0;
    screen->terms[i].y = 0;
    i++;
  }
  /* Draw the screen.  */
  for (i = 0; i < screen->nterms; i++)
    grub_menu_init_page (0, 1, screen->terms[i].term);
  update_screen_all (screen, 0, 0, 1, 1, ALL_LINES);
  for (i = 0; i < screen->nterms; i++)
    grub_term_setcursor (screen->terms[i].term, 1);
  prev_c = '\0';

  while (1)
    {
      int c = GRUB_TERM_ASCII_CHAR (grub_getkey ());

      if (screen->completion_shown)
	{
	  clear_completions_all (screen);
	  screen->completion_shown = 0;
	}

      if (grub_normal_exit_level)
	{
	  destroy_screen (screen);
	  return;
	}

      switch (c)
	{
	case 16: /* C-p */
	  if (! previous_line (screen, 1))
	    goto fail;
	  break;

	case 14: /* C-n */
	  if (! next_line (screen, 1))
	    goto fail;
	  break;

	case 6: /* C-f */
	  if (! forward_char (screen, 1))
	    goto fail;
	  break;

	case 2: /* C-b */
	  if (! backward_char (screen, 1))
	    goto fail;
	  break;

	case 1: /* C-a */
	  if (! beginning_of_line (screen, 1))
	    goto fail;
	  break;

	case 5: /* C-e */
	  if (! end_of_line (screen, 1))
	    goto fail;
	  break;

	case '\t': /* C-i */
	  if (! complete (screen, prev_c == c, 1))
	    goto fail;
	  break;

	case 4: /* C-d */
	  if (! delete_char (screen, 1))
	    goto fail;
	  break;

	case 8: /* C-h */
	  if (! backward_delete_char (screen, 1))
	    goto fail;
	  break;

	case 11: /* C-k */
	  if (! kill_line (screen, prev_c == c, 1))
	    goto fail;
	  break;

	case 21: /* C-u */
	  /* FIXME: What behavior is good for this key?  */
	  break;

	case 25: /* C-y */
	  if (! yank (screen, 1))
	    goto fail;
	  break;

	case 12: /* C-l */
	  /* FIXME: centering.  */
	  goto refresh;

	case 15: /* C-o */
	  if (! open_line (screen, 1))
	    goto fail;
	  break;

	case '\n':
	case '\r':
	  if (! insert_string (screen, "\n", 1))
	    goto fail;
	  break;

	case '\e':
	  destroy_screen (screen);
	  return;

	case 3: /* C-c */
	  grub_cmdline_run (1);
	  goto refresh;

	case 24: /* C-x */
	  if (! run (screen))
	    goto fail;
	  goto refresh;

	case 18: /* C-r */
	case 19: /* C-s */
	case 20: /* C-t */
	  /* FIXME */
	  break;

	default:
	  if (grub_isprint (c))
	    {
	      char buf[2];

	      buf[0] = c;
	      buf[1] = '\0';
	      if (! insert_string (screen, buf, 1))
		goto fail;
	    }
	  break;
	}

      prev_c = c;
    }

 fail:
  destroy_screen (screen);

  grub_cls ();
  grub_print_error ();
  grub_errno = GRUB_ERR_NONE;
  grub_putchar ('\n');
  grub_printf_ (N_("Press any key to continue..."));
  (void) grub_getkey ();
}
