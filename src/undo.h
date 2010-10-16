#ifndef EOF_UNDO_H
#define EOF_UNDO_H

#include "main.h"
#include "note.h"
#include "beat.h"
#include "event.h"

#define EOF_MAX_UNDO 8
#define EOF_UNDO_TYPE_NONE         0
#define EOF_UNDO_TYPE_NOTE_LENGTH  1
#define EOF_UNDO_TYPE_NOTE_SEL     2
#define EOF_UNDO_TYPE_LYRIC_NOTE   3
#define EOF_UNDO_TYPE_RECORD       4
#define EOF_UNDO_TYPE_SILENCE      5

/*typedef struct
{

	EOF_SONG_TAGS tags;
	EOF_TRACK     track;
	EOF_BEAT_MARKER beat[EOF_MAX_BEATS];
	EOF_TEXT_EVENT event[EOF_MAX_TEXT_EVENTS];
	int events;
	char utrack;

} EOF_UNDO_STATE; */

extern int eof_undo_count;
extern int eof_redo_count;
extern int eof_undo_last_type;

void eof_undo_reset(void);	//Clears all undo states
int eof_undo_add(int type);	//Adds another undo state, except some cases where the requested undo type is the same as the previous undo (ie. a note addition/deletion)
int eof_undo_apply(void);	//Saves the redo state and applies the next available undo state
void eof_redo_apply(void);	//Applies the redo state

int eof_undo_load_state(const char * fn);	//Applies the specified undo/redo state, or returns 0 upon error (called by eof_undo_apply() and eof_redo_apply())

#endif