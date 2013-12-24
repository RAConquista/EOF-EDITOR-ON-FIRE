#include <allegro.h>
#include <time.h>
#include "agup/agup.h"
#include "beat.h"
#include "chart_import.h"	//For FindLongestLineLength_ALLEGRO()
#include "dialog.h"
#include "event.h"
#include "main.h"
#include "midi.h"
#include "mix.h"	//For eof_set_seek_position()
#include "rs.h"
#include "rs_import.h"	//For eof_parse_chord_template()
#include "song.h"	//For eof_pro_guitar_track_delete_hand_position()
#include "tuning.h"	//For eof_lookup_tuned_note()
#include "undo.h"
#include "utility.h"	//For eof_system()
#include "foflc/RS_parse.h"	//For expand_xml_text()
#include "menu/beat.h"	//For eof_rocksmith_phrase_dialog_add()
#include "menu/song.h"	//For eof_menu_track_selected_track_number()
#include "menu/track.h"	//For eof_fret_hand_position_list_dialog_undo_made and eof_fret_hand_position_list_dialog[]

#ifdef USEMEMWATCH
#include "memwatch.h"
#endif

EOF_CHORD_SHAPE eof_chord_shape[EOF_MAX_CHORD_SHAPES];	//An array storing chord shape definitions
unsigned long num_eof_chord_shapes = 0;	//Defines how many chord shapes are defined in eof_chord_shape[]

EOF_RS_PREDEFINED_SECTION eof_rs_predefined_sections[EOF_NUM_RS_PREDEFINED_SECTIONS] =
{
	{"intro", "Intro"},
	{"outro", "Outro"},
	{"verse", "Verse"},
	{"chorus", "Chorus"},
	{"bridge", "Bridge"},
	{"solo", "Solo"},
	{"ambient", "Ambient"},
	{"breakdown", "Breakdown"},
	{"interlude", "Interlude"},
	{"prechorus", "Pre Chorus"},
	{"transition", "Transition"},
	{"postchorus", "Post Chorus"},
	{"hook", "Hook"},
	{"riff", "Riff"},
	{"fadein", "Fade In"},
	{"fadeout", "Fade Out"},
	{"buildup", "Buildup"},
	{"preverse", "Pre Verse"},
	{"modverse", "Modulated Verse"},
	{"postvs", "Post Verse"},
	{"variation", "Variation"},
	{"modchorus", "Modulated Chorus"},
	{"head", "Head"},
	{"modbridge", "Modulated Bridge"},
	{"melody", "Melody"},
	{"postbrdg", "Post Bridge"},
	{"prebrdg", "Pre Bridge"},
	{"vamp", "Vamp"},
	{"noguitar", "No Guitar"},
	{"silence", "Silence"}
};

EOF_RS_PREDEFINED_SECTION eof_rs_predefined_events[EOF_NUM_RS_PREDEFINED_EVENTS] =
{
	{"B0", "High pitch tick (B0)"},
	{"B1", "Low pitch tick (B1)"},
	{"E1", "Crowd happy (E1)"},
	{"E3", "Crowd wild (E3)"}
};

unsigned char *eof_fret_range_tolerances = NULL;	//A dynamically allocated array that defines the fretting hand's range for each fret on the guitar neck, numbered where fret 1's range is defined at eof_fret_range_tolerances[1]

char *eof_rs_arrangement_names[5] = {"Undefined", "Combo", "Rhythm", "Lead", "Bass"};	//Indexes 1 through 4 represent the valid arrangement names for Rocksmith arrangements

int eof_is_string_muted(EOF_SONG *sp, unsigned long track, unsigned long note)
{
	unsigned long ctr, bitmask;
	EOF_PRO_GUITAR_TRACK *tp;
	int allmuted = 1;	//Will be set to nonzero if any used strings aren't fret hand muted

	if(!sp || (track >= sp->tracks) || (note >= eof_get_track_size(sp, track)) || (sp->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT))
		return 0;	//Return error

	tp = sp->pro_guitar_track[sp->track[track]->tracknum];
	for(ctr = 0, bitmask = 1; ctr < 6; ctr++, bitmask <<= 1)
	{	//For each of the 6 supported strings
		if(ctr < tp->numstrings)
		{	//If this is a string used in the track
			if(tp->note[note]->note & bitmask)
			{	//If this is a string used in the note
				if((tp->note[note]->frets[ctr] & 0x80) == 0)
				{	//If this string is not fret hand muted
					allmuted = 0;
					break;
				}
			}
		}
	}

	return allmuted;	//Return nonzero if all of the used strings were fret hand muted
}

int eof_is_partially_ghosted(EOF_SONG *sp, unsigned long track, unsigned long note)
{
	unsigned long ctr, bitmask;
	EOF_PRO_GUITAR_TRACK *tp;
	char ghosted = 0, nonghosted = 0;	//Tracks the number of gems in this note that are ghosted and non ghosted

	if(!sp || (track >= sp->tracks) || (note >= eof_get_track_size(sp, track)) || (sp->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT))
		return 0;	//Return error

	tp = sp->pro_guitar_track[sp->track[track]->tracknum];
	for(ctr = 0, bitmask = 1; ctr < 6; ctr++, bitmask <<= 1)
	{	//For each of the 6 supported strings
		if(ctr < tp->numstrings)
		{	//If this is a string used in the track
			if(tp->note[note]->note & bitmask)
			{	//If this is a string used in the note
				if(tp->note[note]->ghost & bitmask)
				{	//If this string is ghosted
					ghosted++;
				}
				else
				{	//This string is not ghosted
					nonghosted++;
				}
			}
		}
	}

	return (ghosted && nonghosted);	//Return nonzero if the note contained at least one ghosted gem AND one non ghosted gem
}

unsigned long eof_build_chord_list(EOF_SONG *sp, unsigned long track, unsigned long **results, char target)
{
	unsigned long ctr, ctr2, unique_count = 0;
	EOF_PRO_GUITAR_NOTE **notelist;	//An array large enough to hold a pointer to every note in the track
	EOF_PRO_GUITAR_TRACK *tp;
	char match;

	eof_log("eof_rs_build_chord_list() entered", 1);

	if(!results)
		return 0;	//Return error

	if(!sp || (track >= sp->tracks))
	{
		*results = NULL;
		return 0;	//Return error
	}

	//Duplicate the track's note array
	notelist = malloc(sizeof(EOF_PRO_GUITAR_NOTE *) * EOF_MAX_NOTES);	//Allocate memory to duplicate the note[] array
	if(!notelist)
	{
		*results = NULL;
		return 0;	//Return error
	}
	tp = sp->pro_guitar_track[sp->track[track]->tracknum];
	memcpy(notelist, tp->note, sizeof(EOF_PRO_GUITAR_NOTE *) * EOF_MAX_NOTES);	//Copy the note array

	//Overwrite each pointer in the duplicate note array that isn't a unique chord with NULL
	for(ctr = 0; ctr < tp->notes; ctr++)
	{	//For each note in the track
		if(eof_note_count_rs_lanes(sp, track, ctr, target) > 1)
		{	//If this note is a valid chord based on the target
			match = 0;
			for(ctr2 = ctr + 1; ctr2 < tp->notes; ctr2++)
			{	//For each note in the track that follows this note
				if((eof_note_count_rs_lanes(sp, track, ctr2, target) > 1) && !eof_note_compare_simple(sp, track, ctr, ctr2))
				{	//If this note matches one that follows it, and that later note is a valid chord for the target Rocksmith game
					notelist[ctr] = NULL;	//Eliminate this note from the list
					match = 1;	//Note that this chord matched one of the others
					break;
				}
			}
			if(!match)
			{	//If this chord didn't match any of the other notes
				unique_count++;	//Increment unique chord counter
			}
		}//If this note is a valid chord based on the target
		else
		{	//This not is not a chord
			notelist[ctr] = NULL;	//Eliminate this note from the list since it's not a chord
		}
	}//For each note in the track

	if(!unique_count)
	{	//If there were no chords
		*results = NULL;	//Return empty result set
		free(notelist);
		return 0;
	}

	//Allocate and build an array with the note numbers representing the unique chords
	*results = malloc(sizeof(unsigned long) * unique_count);	//Allocate enough memory to store the note number of each unique chord
	if(*results == NULL)
	{
		free(notelist);
		return 0;
	}
	for(ctr = 0, ctr2 = 0; ctr < tp->notes; ctr++)
	{	//For each note in the track
		if(notelist[ctr] != NULL)
		{	//If this was a unique chord
			if(ctr2 < unique_count)
			{	//Bounds check
				(*results)[ctr2++] = ctr;	//Append the note number
			}
		}
	}

	//Cleanup and return results
	free(notelist);
	return unique_count;
}

unsigned long eof_build_section_list(EOF_SONG *sp, unsigned long **results, unsigned long track)
{
	unsigned long ctr, ctr2, unique_count = 0;
	EOF_TEXT_EVENT **eventlist;	//An array large enough to hold a pointer to every text event in the chart
	char match;

	eof_log("eof_build_section_list() entered", 1);

	if(!results)
		return 0;	//Return error

	if(!sp)
	{
		*results = NULL;
		return 0;	//Return error
	}

	//Duplicate the chart's text events array
	eventlist = malloc(sizeof(EOF_TEXT_EVENT *) * EOF_MAX_TEXT_EVENTS);
	if(!eventlist)
	{
		*results = NULL;
		return 0;	//Return error
	}
	memcpy(eventlist, sp->text_event, sizeof(EOF_TEXT_EVENT *) * EOF_MAX_TEXT_EVENTS);	//Copy the event array

	//In the case of beats that contain multiple sections, only keep ones that are cached in the beat statistics
	eof_process_beat_statistics(sp, track);	//Rebuild beat stats from the perspective of the track being examined
	for(ctr = 0; ctr < sp->text_events; ctr++)
	{	//For each text event in the chart
		match = 0;
		for(ctr2 = 0; ctr2 < sp->beats; ctr2++)
		{	//For each beat in the chart
			if(sp->beat[ctr2]->contained_section_event == ctr)
			{	//If the beat's statistics indicate this section is used
				match = 1;	//Note that this section is to be kept
				break;
			}
		}
		if(!match)
		{	//If none of the beat stats used this section
			eventlist[ctr] = NULL;	//Eliminate this section from the list since only 1 section per beat will be exported
		}
	}

	//Overwrite each pointer in the duplicate event array that isn't a unique section marker with NULL and count the unique events
	for(ctr = 0; ctr < sp->text_events; ctr++)
	{	//For each text event in the chart
		if(eventlist[ctr] != NULL)
		{	//If this section hasn't been eliminated yet
			if(eof_is_section_marker(sp->text_event[ctr], track))
			{	//If the text event's string or flags indicate a section marker (from the perspective of the specified track)
				match = 0;
				for(ctr2 = ctr + 1; ctr2 < sp->text_events; ctr2++)
				{	//For each event in the chart that follows this event
					if(eventlist[ctr2] != NULL)
					{	//If the event wasn't already eliminated
						if(eof_is_section_marker(sp->text_event[ctr2], track) &&!ustricmp(sp->text_event[ctr]->text, sp->text_event[ctr2]->text))
						{	//If this event is also a section event from the perspective of the track being examined, and its text matches
							eventlist[ctr] = NULL;	//Eliminate this event from the list
							match = 1;	//Note that this section matched one of the others
							break;
						}
					}
				}
				if(!match)
				{	//If this section marker didn't match any of the other events
					unique_count++;	//Increment unique section counter
				}
			}
			else
			{	//This event is not a section marker
				eventlist[ctr] = NULL;	//Eliminate this note from the list since it's not a chord
			}
		}
	}

	if(!unique_count)
	{	//If there were no section markers
		*results = NULL;	//Return empty result set
		free(eventlist);
		return 0;
	}

	//Allocate and build an array with the event numbers representing the unique section markers
	*results = malloc(sizeof(unsigned long) * unique_count);	//Allocate enough memory to store the event number of each unique section marker
	if(*results == NULL)
	{
		free(eventlist);
		return 0;
	}
	for(ctr = 0, ctr2 = 0; ctr < sp->text_events; ctr++)
	{	//For each event in the chart
		if(eventlist[ctr] != NULL)
		{	//If this was a unique section marker
			if(ctr2 < unique_count)
			{	//Bounds check
				(*results)[ctr2++] = ctr;	//Append the event number
			}
		}
	}

	//Cleanup and return results
	free(eventlist);
	return unique_count;
}

int eof_song_qsort_control_events(const void * e1, const void * e2)
{
	EOF_RS_CONTROL * thing1 = (EOF_RS_CONTROL *)e1;
	EOF_RS_CONTROL * thing2 = (EOF_RS_CONTROL *)e2;

	//Sort by timestamp
	if(thing1->pos < thing2->pos)
	{
		return -1;
	}
	else if(thing1->pos > thing2->pos)
	{
		return 1;
	}

	//They are equal
	return 0;
}

int eof_export_rocksmith_1_track(EOF_SONG * sp, char * fn, unsigned long track, char *user_warned)
{
	PACKFILE * fp;
	char buffer[600] = {0}, buffer2[512] = {0};
	time_t seconds;		//Will store the current time in seconds
	struct tm *caltime;	//Will store the current time in calendar format
	unsigned long ctr, ctr2, ctr3, ctr4, ctr5, numsections, stringnum, bitmask, numsinglenotes, numchords, *chordlist, chordlistsize, *sectionlist, sectionlistsize, xml_end, numevents = 0;
	EOF_PRO_GUITAR_TRACK *tp;
	char *arrangement_name;	//This will point to the track's native name unless it has an alternate name defined
	unsigned numdifficulties;
	unsigned char bre_populated = 0;
	unsigned long phraseid;
	unsigned beatspermeasure = 4, beatcounter = 0;
	long displayedmeasure, measurenum = 0;
	long startbeat;	//This will indicate the first beat containing a note in the track
	long endbeat;	//This will indicate the first beat after the exported track's last note
	char standard[] = {0,0,0,0,0,0};
	char standardbass[] = {0,0,0,0};
	char eb[] = {-1,-1,-1,-1,-1,-1};
	char dropd[] = {-2,0,0,0,0,0};
	char openg[] = {-2,-2,0,0,0,-2};
	char *tuning;
	char isebtuning = 1;	//Will track whether all strings are tuned to -1
	char notename[EOF_NAME_LENGTH+1];	//String large enough to hold any chord name supported by EOF
	int scale, chord, isslash, bassnote;	//Used for power chord detection
	int standard_tuning = 0, non_standard_chords = 0, barre_chords = 0, power_chords = 0, notenum, dropd_tuning = 1, dropd_power_chords = 0, open_chords = 0, double_stops = 0, palm_mutes = 0, harmonics = 0, hopo = 0, tremolo = 0, slides = 0, bends = 0, tapping = 0, vibrato = 0, slappop = 0, octaves = 0, fifths_and_octaves = 0;	//Used for technique detection
	char is_bass = 0;	//Is set to nonzero if the specified track is to be considered a bass guitar track
	char end_phrase_found = 0;	//Will track if there was a manually defined END phrase
	unsigned long chordid, handshapectr;
	unsigned long handshapestart, handshapeend;
	long nextnote;
	unsigned long originalbeatcount;	//If beats are padded to reach the beginning of the next measure (for DDC), this will track the project's original number of beats

	eof_log("eof_export_rocksmith_1_track() entered", 1);

	if(!sp || !fn || !sp->beats || (track >= sp->tracks) || (sp->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT) || !sp->track[track]->name || !user_warned)
	{
		eof_log("\tError saving:  Invalid parameters", 1);
		return 0;	//Return failure
	}

	tp = sp->pro_guitar_track[sp->track[track]->tracknum];
	if(eof_get_highest_fret(sp, track, 0) > 22)
	{	//If the track being exported uses any frets higher than 22
		if((*user_warned & 2) == 0)
		{	//If the user wasn't alerted about this issue yet
			allegro_message("Warning:  At least one track (\"%s\") uses a fret higher than 22.  This will cause Rocksmith to crash.", sp->track[track]->name);
			*user_warned |= 2;
		}
	}
	for(ctr = 0; ctr < eof_get_track_size(sp, track); ctr++)
	{	//For each note in the track
		unsigned char slideend;

		if(eof_note_count_rs_lanes(sp, track, ctr, 1) == 1)
		{	//If the note will export as a single note
			if(eof_get_note_flags(sp, track, ctr) & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP)
			{	//If the note slides up
				slideend = tp->note[ctr]->slideend + tp->capo;	//Obtain the end position of the slide, take the capo position into account
				if(!(eof_get_note_flags(sp, track, ctr) & EOF_PRO_GUITAR_NOTE_FLAG_RS_NOTATION))
				{	//If this slide's end position is not defined
					slideend = eof_get_highest_fret_value(sp, track, ctr) + 1;	//Assume a 1 fret slide
				}
				if(slideend >= 22)
				{	//If the slide goes to or above fret 22
					if((*user_warned & 8) == 0)
					{	//If the user wasn't alerted about this issue yet
						eof_seek_and_render_position(track, tp->note[ctr]->type, tp->note[ctr]->pos);	//Show the offending note
						allegro_message("Warning:  At least one note slides to or above fret 22.  This will cause Rocksmith 1 to crash.");
						*user_warned |= 8;
					}
					break;
				}
			}
		}
	}

	//Count the number of populated difficulties in the track
	(void) eof_detect_difficulties(sp, track);	//Update eof_track_diff_populated_status[] to reflect all populated difficulties for this track
	if((sp->track[track]->flags & EOF_TRACK_FLAG_UNLIMITED_DIFFS) == 0)
	{	//If the track is using the traditional 5 difficulty system
		if(eof_track_diff_populated_status[4])
		{	//If the BRE difficulty is populated
			bre_populated = 1;	//Track that it was
		}
		eof_track_diff_populated_status[4] = 0;	//Ensure that the BRE difficulty is not exported
	}
	for(ctr = 0, numdifficulties = 0; ctr < 256; ctr++)
	{	//For each possible difficulty
		if(eof_track_diff_populated_status[ctr])
		{	//If this difficulty is populated
			numdifficulties++;	//Increment this counter
		}
	}
	if(!numdifficulties)
	{
		(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "Cannot export track \"%s\"in Rocksmith format, it has no populated difficulties", sp->track[track]->name);
		eof_log(eof_log_string, 1);
		if(bre_populated)
		{	//If the BRE difficulty was the only one populated, warn that it is being omitted
			(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "Warning:  Track \"%s\" only has notes in the BRE difficulty.\nThese are not exported in Rocksmith format unless you remove the difficulty limit (Song>Rocksmith>Remove difficulty limit).", sp->track[track]->name);
			allegro_message(eof_log_string);
			eof_log(eof_log_string, 1);
		}
		return 0;	//Return failure
	}

	//Update target file name and open it for writing
	if((sp->track[track]->flags & EOF_TRACK_FLAG_ALT_NAME) && (sp->track[track]->altname[0] != '\0'))
	{	//If the track has an alternate name
		arrangement_name = sp->track[track]->altname;
	}
	else
	{	//Otherwise use the track's native name
		arrangement_name = sp->track[track]->name;
	}
	(void) snprintf(buffer, 600, "%s.xml", arrangement_name);
	(void) replace_filename(fn, fn, buffer, 1024);
	fp = pack_fopen(fn, "w");
	if(!fp)
	{
		eof_log("\tError saving:  Cannot open file for writing", 1);
		return 0;	//Return failure
	}

	//Update the track's arrangement name
	if(tp->arrangement)
	{	//If the track's arrangement type has been defined
		arrangement_name = eof_rs_arrangement_names[tp->arrangement];	//Use the corresponding string in the XML file
	}

	//Get the smaller of the chart length and the music length, this will be used to write the songlength tag
	xml_end = eof_music_length;
	if(eof_silence_loaded || (eof_chart_length < eof_music_length))
	{	//If the chart length is shorter than the music length, or there is no chart audio loaded
		xml_end = eof_chart_length;	//Use the chart's length instead
	}

	//Write the beginning of the XML file
	(void) pack_fputs("<?xml version='1.0' encoding='UTF-8'?>\n", fp);
	(void) pack_fputs("<song version=\"4\">\n", fp);
	(void) pack_fputs("<!-- " EOF_VERSION_STRING " -->\n", fp);	//Write EOF's version in an XML comment
	expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->tags->title, 64);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <title>%s</title>\n", buffer2);
	(void) pack_fputs(buffer, fp);
	expand_xml_text(buffer2, sizeof(buffer2) - 1, arrangement_name, 32);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <arrangement>%s</arrangement>\n", buffer2);
	(void) pack_fputs(buffer, fp);
	(void) pack_fputs("  <part>1</part>\n", fp);
	(void) pack_fputs("  <offset>0.000</offset>\n", fp);
	eof_truncate_chart(sp);	//Update the chart length
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <songLength>%.3f</songLength>\n", (double)(xml_end - 1) / 1000.0);	//Make sure the song length is not longer than the actual audio, or the chart won't reach an end in-game
	(void) pack_fputs(buffer, fp);
	seconds = time(NULL);
	caltime = localtime(&seconds);
	if(caltime)
	{	//If the calendar time could be determined
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <lastConversionDateTime>%d-%d-%d %d:%02d</lastConversionDateTime>\n", caltime->tm_mon + 1, caltime->tm_mday, caltime->tm_year % 100, caltime->tm_hour % 12, caltime->tm_min);
	}
	else
	{
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <lastConversionDateTime>UNKNOWN</lastConversionDateTime>\n");
	}
	(void) pack_fputs(buffer, fp);

	//Write additional tags to pass song information to the Rocksmith toolkit
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <startBeat>%.3f</startBeat>\n", sp->beat[0]->fpos / 1000.0);	//The position of the first beat
	(void) pack_fputs(buffer, fp);
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <averageTempo>%.3f</averageTempo>\n", 60000.0 / ((sp->beat[sp->beats - 1]->fpos - sp->beat[0]->fpos) / sp->beats));	//The average tempo (60000ms / the average beat length in ms)
	(void) pack_fputs(buffer, fp);
	tuning = tp->tuning;	//By default, use the track's original tuning array
	for(ctr = 0; ctr < 6; ctr++)
	{	//For each string EOF supports
		if(ctr >= tp->numstrings)
		{	//If the track doesn't use this string
			tp->tuning[ctr] = 0;	//Ensure the tuning is cleared accordingly
		}
	}
	for(ctr = 0; ctr < tp->numstrings; ctr++)
	{	//For each string in this track
		if(tp->tuning[ctr] != -1)
		{	//If this string isn't tuned a half step down
			isebtuning = 0;
			break;
		}
	}
	is_bass = eof_track_is_bass_arrangement(tp, track);
	if(isebtuning && !(is_bass && (tp->numstrings > 4)))
	{	//If all strings were tuned down a half step (except for bass tracks with more than 4 strings, since in those cases, the lowest string is not tuned to E)
		tuning = eb;	//Remap 4 or 5 string Eb tuning as {-1,-1,-1,-1,-1,-1}
	}
	if(memcmp(tuning, standard, 6) && memcmp(tuning, standardbass, 4) && memcmp(tuning, eb, 6) && memcmp(tuning, dropd, 6) && memcmp(tuning, openg, 6))
	{	//If the track's tuning doesn't match any supported by Rocksmith
		allegro_message("Warning:  This track (%s) uses a tuning that isn't one known to be supported in Rocksmith.\nTuning and note recognition may not work as expected in-game", sp->track[track]->name);
		(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "Warning:  This track (%s) uses a tuning that isn't known to be supported in Rocksmith.  Tuning and note recognition may not work as expected in-game", sp->track[track]->name);
		eof_log(eof_log_string, 1);
	}
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <tuning string0=\"%d\" string1=\"%d\" string2=\"%d\" string3=\"%d\" string4=\"%d\" string5=\"%d\" />\n", tuning[0], tuning[1], tuning[2], tuning[3], tuning[4], tuning[5]);
	(void) pack_fputs(buffer, fp);
	expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->tags->artist, 256);	//Replace any special characters in the artist song property with escape sequences if necessary
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <artistName>%s</artistName>\n", buffer2);
	(void) pack_fputs(buffer, fp);
	expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->tags->album, 256);	//Replace any special characters in the album song property with escape sequences if necessary
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <albumName>%s</albumName>\n", buffer2);
	(void) pack_fputs(buffer, fp);
	expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->tags->year, 32);	//Replace any special characters in the year song property with escape sequences if necessary
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <albumYear>%s</albumYear>\n", buffer2);
	(void) pack_fputs(buffer, fp);

	//Determine arrangement properties
	if(!memcmp(tuning, standard, 6))
	{	//All unused strings had their tuning set to 0, so if all bytes of this array are 0, the track is in standard tuning
		standard_tuning = 1;
	}
	notenum = eof_lookup_tuned_note(tp, track, 0, tp->tuning[0]);	//Look up the open note the lowest string plays
	notenum %= 12;	//Ensure the value is in the range of [0,11]
	if(notenum == 5)
	{	//If the lowest string is tuned to D
		for(ctr = 1; ctr < 6; ctr++)
		{	//For the other 5 strings
			if(tp->tuning[ctr] != 0)
			{	//If the string is not in standard tuning
				dropd_tuning = 0;
			}
		}
	}
	else
	{	//The lowest string is not tuned to D
		dropd_tuning = 0;
	}
	eof_determine_phrase_status(sp, track);	//Update the tremolo status of each note
	for(ctr = 0; ctr < tp->notes; ctr++)
	{	//For each note in the track
		if(eof_note_count_rs_lanes(sp, track, ctr, 1) > 1)
		{	//If the note will export as a chord (more than one non ghosted/muted gem)
			if(!non_standard_chords && !eof_build_note_name(sp, track, ctr, notename))
			{	//If the chord has no defined or detected name (only if this condition hasn't been found already)
				non_standard_chords = 1;
			}
			if(!barre_chords && eof_pro_guitar_note_is_barre_chord(tp, ctr))
			{	//If the chord is a barre chord (only if this condition hasn't been found already)
				barre_chords = 1;
			}
			if(!power_chords && eof_lookup_chord(tp, track, ctr, &scale, &chord, &isslash, &bassnote, 0, 0))
			{	//If the chord lookup found a match (only if this condition hasn't been found already)
				if(chord == 27)
				{	//27 is the index of the power chord formula in eof_chord_names[]
					power_chords = 1;
					if(dropd_tuning)
					{	//If this track is in drop d tuning
						dropd_power_chords = 1;
					}
				}
			}
			if(!open_chords)
			{	//Only if no open chords have been found already
				for(ctr2 = 0, bitmask = 1; ctr2 < 6; ctr2++, bitmask <<= 1)
				{	//For each of the 6 supported strings
					if((tp->note[ctr]->note & bitmask) && (tp->note[ctr]->frets[ctr2] == 0))
					{	//If this string is used and played open
						open_chords = 1;
					}
				}
			}
			if(!double_stops && eof_pro_guitar_note_is_double_stop(tp, ctr))
			{	//If the chord is a double stop (only if this condition hasn't been found already)
				double_stops = 1;
			}
			if(is_bass)
			{	//If the arrangement being exported is bass
				int thisnote, lastnote = -1, failed = 0;
				for(ctr2 = 0, bitmask = 1; ctr2 < 6; ctr2++, bitmask <<= 1)
				{	//For each of the 6 supported strings
					if((tp->note[ctr]->note & bitmask) && !(tp->note[ctr]->frets[ctr2] & 0x80))
					{	//If this string is played and not string muted
						thisnote = eof_lookup_played_note(tp, track, ctr2, tp->note[ctr]->frets[ctr2]);	//Determine what note is being played
						if((lastnote >= 0) && (lastnote != thisnote))
						{	//If this string's played note doesn't match the note played by previous strings
							failed = 1;
							break;
						}
						lastnote = thisnote;
					}
				}
				if(!failed)
				{	//If non muted strings in the chord played the same note
					octaves = 1;
				}
			}
		}//If the note is a chord (more than one non ghosted gem)
		if(tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_PALM_MUTE)
		{	//If the note is palm muted
			palm_mutes = 1;
		}
		if(tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_HARMONIC)
		{	//If the note is a harmonic
			harmonics = 1;
		}
		if((tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_HO) || (tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_PO))
		{	//If the note is a hammer on or pull off
			hopo = 1;
		}
		if(tp->note[ctr]->flags & EOF_NOTE_FLAG_IS_TREMOLO)
		{	//If the note is played with tremolo
			tremolo = 1;
		}
		if((tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP) || (tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN))
		{	//If the note slides up or down
			slides = 1;
		}
		if(tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_BEND)
		{	//If the note is bent
			bends = 1;
		}
		if(tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_TAP)
		{	//If the note is tapped
			tapping = 1;
		}
		if(tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_VIBRATO)
		{	//If the note is played with vibrato
			vibrato = 1;
		}
		if((tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_SLAP) || (tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_POP))
		{	//If the note is played by slapping or popping
			slappop = 1;
		}
	}//For each note in the track
	if(is_bass)
	{	//If the arrangement being exported is bass
		if(double_stops || octaves)
		{	//If either of these techniques were detected
			fifths_and_octaves = 1;
		}
		double_stops = 0;
	}
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <arrangementProperties represent=\"1\" standardTuning=\"%d\" nonStandardChords=\"%d\" barreChords=\"%d\" powerChords=\"%d\" dropDPower=\"%d\" openChords=\"%d\" fingerPicking=\"0\" pickDirection=\"0\" doubleStops=\"%d\" palmMutes=\"%d\" harmonics=\"%d\" pinchHarmonics=\"0\" hopo=\"%d\" tremolo=\"%d\" slides=\"%d\" unpitchedSlides=\"0\" bends=\"%d\" tapping=\"%d\" vibrato=\"%d\" fretHandMutes=\"0\" slapPop=\"%d\" twoFingerPicking=\"0\" fifthsAndOctaves=\"%d\" syncopation=\"0\" bassPick=\"0\" />\n", standard_tuning, non_standard_chords, barre_chords, power_chords, dropd_power_chords, open_chords, double_stops, palm_mutes, harmonics, hopo, tremolo, slides, bends, tapping, vibrato, slappop, fifths_and_octaves);
	(void) pack_fputs(buffer, fp);

	//Check if any RS phrases or sections need to be added
	startbeat = eof_get_beat(sp, tp->note[0]->pos);	//Find the beat containing the track's first note
	if(startbeat < 0)
	{	//If the beat couldn't be found
		startbeat = 0;	//Set this to the first beat
	}
	endbeat = eof_get_beat(sp, tp->note[tp->notes - 1]->pos + tp->note[tp->notes - 1]->length);	//Find the beat containing the end of the track's last note
	if((endbeat < 0) || (endbeat + 1 >= sp->beats))
	{	//If the beat couldn't be found, or the extreme last beat in the track has the last note
		endbeat = sp->beats - 1;	//Set this to the last beat
	}
	else
	{
		endbeat++;	//Otherwise set it to the first beat that follows the end of the last note
	}
	eof_process_beat_statistics(sp, track);	//Cache section name information into the beat structures (from the perspective of the specified track)
	if(!eof_song_contains_event(sp, "COUNT", track, EOF_EVENT_FLAG_RS_PHRASE, 1) && !eof_song_contains_event(sp, "COUNT", 0, EOF_EVENT_FLAG_RS_PHRASE, 1))
	{	//If the user did not define a COUNT phrase that applies to either the track being exported or all tracks
		if(sp->beat[0]->contained_section_event >= 0)
		{	//If there is already a phrase defined on the first beat
			allegro_message("Warning:  There is no COUNT phrase, but the first beat marker already has a phrase.\nYou should move that phrase because only one phrase per beat is exported.");
		}
		eof_log("\t! Adding missing COUNT phrase", 1);
		(void) eof_song_add_text_event(sp, 0, "COUNT", 0, EOF_EVENT_FLAG_RS_PHRASE, 1);	//Add it as a temporary event at the first beat
	}
	for(ctr = 0; ctr < sp->beats; ctr++)
	{	//For each beat
		if((sp->beat[ctr]->contained_section_event >= 0) && !ustricmp(sp->text_event[sp->beat[ctr]->contained_section_event]->text, "END"))
		{	//If this beat contains an "END" RS phrase
			for(ctr2 = ctr + 1; ctr2 < sp->beats; ctr2++)
			{	//For each remaining beat
				if((sp->beat[ctr2]->contained_section_event >= 0) || (sp->beat[ctr2]->contained_rs_section_event >= 0))
				{	//If the beat contains an RS phrase or RS section
					eof_2d_render_top_option = 36;	//Change the user preference to display RS phrases and sections
					eof_selected_beat = ctr;		//Select it
					eof_seek_and_render_position(track, eof_note_type, sp->beat[ctr]->pos);	//Show the offending END phrase
					allegro_message("Warning:  Beat #%lu contains an END phrase, but there's at least one more phrase or section after it.\nThis will cause dynamic difficulty and/or riff repeater to not work correctly.", ctr);
					break;
				}
			}
			end_phrase_found = 1;
			break;
		}
	}
	if(!end_phrase_found)
	{	//If the user did not define a END phrase
		if(sp->beat[endbeat]->contained_section_event >= 0)
		{	//If there is already a phrase defined on the beat following the last note
			eof_2d_render_top_option = 36;	//Change the user preference to display RS phrases and sections
			eof_selected_beat = endbeat;		//Select it
			eof_seek_and_render_position(track, eof_note_type, sp->beat[endbeat]->pos);	//Show where the END phrase should go
			allegro_message("Warning:  There is no END phrase, but the beat marker after the last note in \"%s\" already has a phrase.\nYou should move that phrase because only one phrase per beat is exported.", sp->track[track]->name);
		}
		eof_log("\t! Adding missing END phrase", 1);
		(void) eof_song_add_text_event(sp, endbeat, "END", 0, EOF_EVENT_FLAG_RS_PHRASE, 1);	//Add it as a temporary event at the last beat
	}
	eof_sort_events(sp);	//Re-sort events
	eof_process_beat_statistics(sp, track);	//Cache section name information into the beat structures (from the perspective of the specified track)
	if(!eof_song_contains_event(sp, "intro", track, EOF_EVENT_FLAG_RS_SECTION, 1) && !eof_song_contains_event(sp, "intro", 0, EOF_EVENT_FLAG_RS_SECTION, 1))
	{	//If the user did not define an intro RS section that applies to either the track being exported or all tracks
		if(sp->beat[startbeat]->contained_rs_section_event >= 0)
		{	//If there is already a RS section defined on the first beat containing a note
			allegro_message("Warning:  There is no intro RS section, but the beat marker before the first note already has a section.\nYou should move that section because only one section per beat is exported.");
		}
		eof_log("\t! Adding missing intro RS section", 1);
		(void) eof_song_add_text_event(sp, startbeat, "intro", 0, EOF_EVENT_FLAG_RS_SECTION, 1);	//Add a temporary one
	}
	if(!eof_song_contains_event(sp, "noguitar", track, EOF_EVENT_FLAG_RS_SECTION, 1) && !eof_song_contains_event(sp, "noguitar", 0, EOF_EVENT_FLAG_RS_SECTION, 1))
	{	//If the user did not define a noguitar RS section that applies to either the track being exported or all tracks
		if(sp->beat[endbeat]->contained_rs_section_event >= 0)
		{	//If there is already a RS section defined on the first beat after the last note
			allegro_message("Warning:  There is no noguitar RS section, but the beat marker after the last note already has a section.\nYou should move that section because only one section per beat is exported.");
		}
		eof_log("\t! Adding missing noguitar RS section", 1);
		(void) eof_song_add_text_event(sp, endbeat, "noguitar", 0, EOF_EVENT_FLAG_RS_SECTION, 1);	//Add a temporary one
	}

	//Write the phrases
	eof_sort_events(sp);	//Re-sort events
	eof_process_beat_statistics(sp, track);	//Cache section name information into the beat structures (from the perspective of the specified track)
	for(ctr = 0, numsections = 0; ctr < sp->beats; ctr++)
	{	//For each beat in the chart
		if(sp->beat[ctr]->contained_section_event >= 0)
		{	//If this beat has a section event (RS phrase)
			numsections++;	//Update section marker instance counter
		}
	}
	sectionlistsize = eof_build_section_list(sp, &sectionlist, track);	//Build a list of all unique section markers (Rocksmith phrases) in the chart (from the perspective of the track being exported)
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <phrases count=\"%lu\">\n", sectionlistsize);	//Write the number of unique phrases
	(void) pack_fputs(buffer, fp);
	for(ctr = 0; ctr < sectionlistsize; ctr++)
	{	//For each of the entries in the unique section (RS phrase) list
		char * currentphrase = NULL;
		unsigned long startpos = 0, endpos = 0;		//Track the start and end position of the each instance of the phrase
		unsigned char maxdiff, ongoingmaxdiff = 0;	//Track the highest fully leveled difficulty used among all phraseinstances
		char started = 0;

		//Determine the highest maxdifficulty present among all instances of this phrase
		for(ctr2 = 0; ctr2 < sp->beats; ctr2++)
		{	//For each beat
			if((sp->beat[ctr2]->contained_section_event >= 0) || ((ctr2 + 1 >= eof_song->beats) && started))
			{	//If this beat contains a section event (Rocksmith phrase) or a phrase is in progress and this is the last beat, it marks the end of any current phrase and the potential start of another
				started = 0;
				endpos = sp->beat[ctr2]->pos - 1;	//Track this as the end position of the previous phrase marker
				if(currentphrase)
				{	//If the first instance of the phrase was already encountered
					if(!ustricmp(currentphrase, sp->text_event[sectionlist[ctr]]->text))
					{	//If the phrase that just ended is an instance of the phrase being written
						maxdiff = eof_find_fully_leveled_rs_difficulty_in_time_range(sp, track, startpos, endpos, 1);	//Find the maxdifficulty value for this phrase instance, converted to relative numbering
						if(maxdiff > ongoingmaxdiff)
						{	//If that phrase instance had a higher maxdifficulty than the other instances checked so far
							ongoingmaxdiff = maxdiff;	//Track it
						}
					}
				}
				started = 1;
				startpos = sp->beat[ctr2]->pos;	//Track the starting position
				currentphrase = sp->text_event[eof_song->beat[ctr2]->contained_section_event]->text;	//Track which phrase is being examined
			}
		}

		//Write the phrase definition using the highest difficulty found among all instances of the phrase
		expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->text_event[sectionlist[ctr]]->text, 32);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
		(void) snprintf(buffer, sizeof(buffer) - 1, "    <phrase disparity=\"0\" ignore=\"0\" maxDifficulty=\"%u\" name=\"%s\" solo=\"0\"/>\n", ongoingmaxdiff, buffer2);
		(void) pack_fputs(buffer, fp);
	}//For each of the entries in the unique section (RS phrase) list
	(void) pack_fputs("  </phrases>\n", fp);
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <phraseIterations count=\"%lu\">\n", numsections);	//Write the number of phrase instances
	(void) pack_fputs(buffer, fp);
	for(ctr = 0; ctr < sp->beats; ctr++)
	{	//For each beat in the chart
		if(sp->beat[ctr]->contained_section_event >= 0)
		{	//If this beat has a section event
			for(ctr2 = 0; ctr2 < sectionlistsize; ctr2++)
			{	//For each of the entries in the unique section list
				if(!ustricmp(sp->text_event[sp->beat[ctr]->contained_section_event]->text, sp->text_event[sectionlist[ctr2]]->text))
				{	//If this event matches a section marker entry
					phraseid = ctr2;
					break;
				}
			}
			if(ctr2 >= sectionlistsize)
			{	//If the section couldn't be found
				allegro_message("Error:  Couldn't find section in unique section list.  Aborting Rocksmith export.");
				eof_log("Error:  Couldn't find section in unique section list.  Aborting Rocksmith export.", 1);
				free(sectionlist);
				return 0;	//Return error
			}
			(void) snprintf(buffer, sizeof(buffer) - 1, "    <phraseIteration time=\"%.3f\" phraseId=\"%lu\"/>\n", sp->beat[ctr]->fpos / 1000.0, phraseid);
			(void) pack_fputs(buffer, fp);
		}
	}
	(void) pack_fputs("  </phraseIterations>\n", fp);
	if(sectionlistsize)
	{	//If there were any entries in the unique section list
		free(sectionlist);	//Free the list now
	}

	//Write some unknown information
	(void) pack_fputs("  <linkedDiffs count=\"0\"/>\n", fp);
	(void) pack_fputs("  <phraseProperties count=\"0\"/>\n", fp);

	if(sp->tags->rs_chord_technique_export)
	{	//If the user opted to export chord techniques to the Rocksmith XML files
		//Check for chords with techniques, and for each, add a temporary single note with those techniques at the same position
		//Rocksmith will render the single note on top of the chord box so that the player can see which techniques are indicated
		eof_determine_phrase_status(sp, track);	//Update the tremolo status of each note
		for(ctr = tp->notes; ctr > 0; ctr--)
		{	//For each note in the track, in reverse order
			if(eof_note_count_rs_lanes(sp, track, ctr - 1, 1) > 1)
			{	//If this note will export as a chord (at least two non ghosted/muted gems)
				unsigned long target = EOF_PRO_GUITAR_NOTE_FLAG_BEND | EOF_PRO_GUITAR_NOTE_FLAG_HO | EOF_PRO_GUITAR_NOTE_FLAG_HARMONIC | EOF_PRO_GUITAR_NOTE_FLAG_PALM_MUTE | EOF_PRO_GUITAR_NOTE_FLAG_POP | EOF_PRO_GUITAR_NOTE_FLAG_SLAP | EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP | EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN | EOF_NOTE_FLAG_IS_TREMOLO;	//A list of all statuses to try to notate for chords
				unsigned long bitmask;
				EOF_PRO_GUITAR_NOTE *new_note;

				if(tp->note[ctr - 1]->flags & target)
				{	//If this note has any of the statuses that can be displayed in Rocksmith for single notes
					for(ctr2 = 0, bitmask = 1; ctr2 < 6; ctr2++, bitmask <<= 1)
					{	//For each of the six supported strings
						if(tp->note[ctr - 1]->note & bitmask)
						{	//If this string is used
							new_note = eof_track_add_create_note(sp, track, bitmask, tp->note[ctr - 1]->pos, tp->note[ctr - 1]->length, tp->note[ctr - 1]->type, NULL);	//Initialize a new single note at this position
							if(new_note)
							{	//If the new note was created
								new_note->flags = tp->note[ctr - 1]->flags;					//Clone the flags
								new_note->tflags |= EOF_NOTE_TFLAG_TEMP;					//Mark the note as temporary
								new_note->bendstrength = tp->note[ctr - 1]->bendstrength;	//Copy the bend strength
								new_note->slideend = tp->note[ctr - 1]->slideend;			//And the slide end position
								new_note->frets[ctr2] = tp->note[ctr - 1]->frets[ctr2];		//And this string's fret value
							}
							break;
						}
					}
				}
			}
		}
		eof_track_sort_notes(sp, track);	//Re-sort the notes
	}//If the user opted to export chord techniques to the Rocksmith XML files

	//Write chord templates
	chordlistsize = eof_build_chord_list(sp, track, &chordlist, 1);	//Build a list of all unique chords in the track
	if(!chordlistsize)
	{	//If there were no chords, write an empty chord template tag
		(void) pack_fputs("  <chordTemplates count=\"0\"/>\n", fp);
	}
	else
	{	//There were chords
		long fret0, fret1, fret2, fret3, fret4, fret5;	//Will store the fret number played on each string (-1 means the string is not played)
		long *fret[6] = {&fret0, &fret1, &fret2, &fret3, &fret4, &fret5};	//Allow the fret numbers to be accessed via array
		char *fingerunused = "-1";
		char *finger0, *finger1, *finger2, *finger3, *finger4, *finger5;	//Each will be set to either fingerunknown or fingerunused
		char **finger[6] = {&finger0, &finger1, &finger2, &finger3, &finger4, &finger5};	//Allow the finger strings to be accessed via array
		char finger0def[2] = "0", finger1def[2] = "1", finger2def[2] = "2", finger3def[2] = "3", finger4def[2] = "4", finger5def[2] = "5";	//Static strings for building manually-defined finger information
		char *fingerdef[6] = {finger0def, finger1def, finger2def, finger3def, finger4def, finger5def};	//Allow the fingerdef strings to be accessed via array
		unsigned long bitmask, shapenum;
		EOF_PRO_GUITAR_NOTE temp;	//Will have a matching chord shape definition's fingering applied to
		unsigned char *effective_fingering;	//Will point to either a note's own finger array or one of that of the temp pro guitar note structure above

		(void) snprintf(buffer, sizeof(buffer) - 1, "  <chordTemplates count=\"%lu\">\n", chordlistsize);
		(void) pack_fputs(buffer, fp);
		for(ctr = 0; ctr < chordlistsize; ctr++)
		{	//For each of the entries in the unique chord list
			notename[0] = '\0';	//Empty the note name string
			(void) eof_build_note_name(sp, track, chordlist[ctr], notename);	//Build the note name (if it exists) into notename[]

			effective_fingering = tp->note[chordlist[ctr]]->finger;	//By default, use the chord list entry's finger array
			memcpy(temp.frets, tp->note[chordlist[ctr]]->frets, 6);	//Clone the fretting of the chord into the temporary note
			temp.note = tp->note[chordlist[ctr]]->note;				//Clone the note mask
			if(eof_pro_guitar_note_fingering_valid(tp, chordlist[ctr]) != 1)
			{	//If the fingering for the note is not fully defined
				if(eof_lookup_chord_shape(tp->note[chordlist[ctr]], &shapenum, 0))
				{	//If a fingering for the chord can be found in the chord shape definitions
					eof_apply_chord_shape_definition(&temp, shapenum);	//Apply the matching chord shape definition's fingering
					effective_fingering = temp.finger;	//Use the matching chord shape definition's finger definitions
				}
			}

			for(ctr2 = 0, bitmask = 1; ctr2 < 6; ctr2++, bitmask <<= 1)
			{	//For each of the 6 supported strings
				if((eof_get_note_note(sp, track, chordlist[ctr]) & bitmask) && (ctr2 < tp->numstrings) && ((tp->note[chordlist[ctr]]->frets[ctr2] & 0x80) == 0))
				{	//If the chord list entry uses this string (verifying that the string number is supported by the track) and the string is not fret hand muted (ghost notes must be allowed so that arpeggio shapes can export)
					*(fret[ctr2]) = tp->note[chordlist[ctr]]->frets[ctr2] & 0x7F;	//Retrieve the fret played on this string (masking out the muting bit)
					*(fret[ctr2]) += tp->capo;	//Apply the capo position
					if(effective_fingering[ctr2])
					{	//If the fingering for this string is defined
						char *temp = fingerdef[ctr2];	//Simplify logic below
						temp[0] = '0' + effective_fingering[ctr2];	//Convert decimal to ASCII
						temp[1] = '\0';	//Truncate string
						*(finger[ctr2]) = temp;
					}
					else
					{	//The fingering is not defined, regardless of whether the string is open or fretted
						*(finger[ctr2]) = fingerunused;		//Write a -1, this will allow the XML to compile even if the chord's fingering is incomplete/undefined
					}
				}
				else
				{	//The chord list entry does not use this string
					*(fret[ctr2]) = -1;
					*(finger[ctr2]) = fingerunused;
				}
			}

			expand_xml_text(buffer2, sizeof(buffer2) - 1, notename, 32);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
			(void) snprintf(buffer, sizeof(buffer) - 1, "    <chordTemplate chordName=\"%s\" finger0=\"%s\" finger1=\"%s\" finger2=\"%s\" finger3=\"%s\" finger4=\"%s\" finger5=\"%s\" fret0=\"%ld\" fret1=\"%ld\" fret2=\"%ld\" fret3=\"%ld\" fret4=\"%ld\" fret5=\"%ld\"/>\n", buffer2, finger0, finger1, finger2, finger3, finger4, finger5, fret0, fret1, fret2, fret3, fret4, fret5);
			(void) pack_fputs(buffer, fp);
		}//For each of the entries in the unique chord list
		(void) pack_fputs("  </chordTemplates>\n", fp);
	}//There were chords

	//Write some unknown information
	(void) pack_fputs("  <fretHandMuteTemplates count=\"0\"/>\n", fp);

	//DDC prefers when the XML pads partially complete measures by adding beats to finish the measure and then going one beat into the next measure
	originalbeatcount = sp->beats;	//Store the original beat count
	if(sp->beat[endbeat]->beat_within_measure)
	{	//If the first beat after the last note in this track isn't the first beat in a measure
		ctr = sp->beat[endbeat]->num_beats_in_measure - sp->beat[endbeat]->beat_within_measure;	//This is how many beats need to be after endbeat
		if(endbeat + ctr > sp->beats)
		{	//If the project doesn't have enough beats to accommodate this padding
			(void) eof_song_append_beats(sp, ctr);	//Append them to the end of the project
		}
	}
	eof_process_beat_statistics(sp, track);	//Rebuild beat stats

	//Write the beat timings
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <ebeats count=\"%lu\">\n", sp->beats);
	(void) pack_fputs(buffer, fp);
	for(ctr = 0; ctr < sp->beats; ctr++)
	{	//For each beat in the chart
		if(eof_get_ts(sp,&beatspermeasure,NULL,ctr) == 1)
		{	//If this beat has a time signature change
			beatcounter = 0;
		}
		if(!beatcounter)
		{	//If this is the first beat in a measure
			measurenum++;
			displayedmeasure = measurenum;
		}
		else
		{	//Otherwise the measure is displayed as -1 to indicate no change from the previous beat's measure number
			displayedmeasure = -1;
		}
		(void) snprintf(buffer, sizeof(buffer) - 1, "    <ebeat time=\"%.3f\" measure=\"%ld\"/>\n", sp->beat[ctr]->fpos / 1000.0, displayedmeasure);
		(void) pack_fputs(buffer, fp);
		beatcounter++;
		if(beatcounter >= beatspermeasure)
		{
			beatcounter = 0;
		}
	}
	(void) pack_fputs("  </ebeats>\n", fp);

	//Restore the original number of beats in the project in case any were added for DDC
	(void) eof_song_resize_beats(sp, originalbeatcount);

	//Write message boxes for the loading text song property (if defined) and each user defined popup message
	if(sp->tags->loading_text[0] != '\0')
	{	//If the loading text is defined
		char expanded_text[512];	//A string to expand the user defined text into
		(void) strftime(expanded_text, sizeof(expanded_text), sp->tags->loading_text, caltime);	//Expand any user defined calendar date/time tokens
		expand_xml_text(buffer2, sizeof(buffer2) - 1, expanded_text, 512);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field

		(void) eof_track_add_section(eof_song, track, EOF_RS_POPUP_MESSAGE, 0, 5100, 10100, 1, buffer2);	//Insert the expanded text as a popup message, setting the flag to nonzero to mark is as temporary
		eof_track_pro_guitar_sort_popup_messages(tp);	//Sort the popup messages
	}
	if(tp->popupmessages || tp->tonechanges)
	{	//If at least one popup message or tone change is to be written
		unsigned long count, controlctr = 0;
		size_t stringlen;
		EOF_RS_CONTROL *controls = NULL;

		//Allocate memory for a list of control events
		count = tp->popupmessages * 2;	//Each popup message needs one control message to display and one to clear
		count += tp->tonechanges;		//Each tone change needs one control message
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <controls count=\"%lu\">\n", count);
		(void) pack_fputs(buffer, fp);
		controls = malloc(sizeof(EOF_RS_CONTROL) * count);	//Allocate memory for a list of Rocksmith control events
		if(!controls)
		{
			eof_log("\tError saving:  Cannot allocate memory for control list", 1);
			return 0;	//Return failure
		}

		//Build the list of control events
		for(ctr = 0; ctr < tp->popupmessages; ctr++)
		{	//For each popup message
			//Add the popup message display control to the list
			expand_xml_text(buffer2, sizeof(buffer2) - 1, tp->popupmessage[ctr].name, EOF_SECTION_NAME_LENGTH);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
			stringlen = (size_t)snprintf(NULL, 0, "    <control time=\"%.3f\" code=\"ShowMessageBox(hint%lu, %s)\"/>\n", tp->popupmessage[ctr].start_pos / 1000.0, ctr + 1, buffer2) + 1;	//Find the number of characters needed to store this string
			controls[controlctr].str = malloc(stringlen + 1);	//Allocate memory to build the string
			if(!controls[controlctr].str)
			{
				eof_log("\tError saving:  Cannot allocate memory for control event", 1);
				while(controlctr > 0)
				{	//Free previously allocated strings
					free(controls[controlctr - 1].str);
					controlctr--;
				}
				free(controls);
				return 0;	//Return failure
			}
			(void) snprintf(controls[controlctr].str, stringlen, "    <control time=\"%.3f\" code=\"ShowMessageBox(hint%lu, %s)\"/>\n", tp->popupmessage[ctr].start_pos / 1000.0, ctr + 1, buffer2);
			controls[controlctr].pos = tp->popupmessage[ctr].start_pos;
			controlctr++;

			//Add the clear message control to the list
			stringlen = (size_t)snprintf(NULL, 0, "    <control time=\"%.3f\" code=\"ClearAllMessageBoxes()\"/>\n", tp->popupmessage[ctr].end_pos / 1000.0) + 1;	//Find the number of characters needed to store this string
			controls[controlctr].str = malloc(stringlen + 1);	//Allocate memory to build the string
			if(!controls[controlctr].str)
			{
				eof_log("\tError saving:  Cannot allocate memory for control event", 1);
				while(controlctr > 0)
				{	//Free previously allocated strings
					free(controls[controlctr - 1].str);
					controlctr--;
				}
				free(controls);
				return 0;	//Return failure
			}
			(void) snprintf(controls[controlctr].str, stringlen, "    <control time=\"%.3f\" code=\"ClearAllMessageBoxes()\"/>\n", tp->popupmessage[ctr].end_pos / 1000.0);
			controls[controlctr].pos = tp->popupmessage[ctr].end_pos;
			controlctr++;
		}
		for(ctr = 0; ctr < tp->tonechanges; ctr++)
		{	//For each tone change
			//Add the tone change control to the list
			stringlen = (size_t)snprintf(NULL, 0, "    <control time=\"%.3f\" code=\"CDlcTone(%s)\"/>\n", tp->tonechange[ctr].start_pos / 1000.0, tp->tonechange[ctr].name) + 1;	//Find the number of characters needed to store this string
			controls[controlctr].str = malloc(stringlen + 1);	//Allocate memory to build the string
			if(!controls[controlctr].str)
			{
				eof_log("\tError saving:  Cannot allocate memory for control event", 1);
				while(controlctr > 0)
				{	//Free previously allocated strings
					free(controls[controlctr - 1].str);
					controlctr--;
				}
				free(controls);
				return 0;	//Return failure
			}
			(void) snprintf(controls[controlctr].str, stringlen, "    <control time=\"%.3f\" code=\"CDlcTone(%s)\"/>\n", tp->tonechange[ctr].start_pos / 1000.0, tp->tonechange[ctr].name);
			controls[controlctr].pos = tp->tonechange[ctr].start_pos;
			controlctr++;
		}

		//Sort, write and free the list of control events
		qsort(controls, (size_t)count, sizeof(EOF_RS_CONTROL), eof_song_qsort_control_events);
		for(ctr = 0; ctr < count; ctr++)
		{	//For each control event
			(void) pack_fputs(controls[ctr].str, fp);	//Write the control event string
			free(controls[ctr].str);	//Free the string
		}
		free(controls);	//Free the array

		(void) pack_fputs("  </controls>\n", fp);

		//Remove any loading text popup that was inserted into the track
		for(ctr = 0; ctr < tp->popupmessages; ctr++)
		{	//For each popup message
			if(tp->popupmessage[ctr].flags)
			{	//If the flags field was made nonzero
				eof_track_pro_guitar_delete_popup_message(tp, ctr);	//Delete this temporary popup message
				break;
			}
		}
	}//If at least one popup message or tone change is to be written

	//Write sections
	for(ctr = 0, numsections = 0; ctr < sp->beats; ctr++)
	{	//For each beat in the chart
		if(sp->beat[ctr]->contained_rs_section_event >= 0)
		{	//If this beat has a Rocksmith section
			numsections++;	//Update Rocksmith section instance counter
		}
	}
	if(numsections)
	{	//If there is at least one Rocksmith section defined in the chart (which should be the case since default ones were inserted earlier if there weren't any)
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <sections count=\"%lu\">\n", numsections);
		(void) pack_fputs(buffer, fp);
		for(ctr = 0; ctr < sp->beats; ctr++)
		{	//For each beat in the chart
			if(sp->beat[ctr]->contained_rs_section_event >= 0)
			{	//If this beat has a Rocksmith section
				expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->text_event[sp->beat[ctr]->contained_rs_section_event]->text, 32);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
				(void) snprintf(buffer, sizeof(buffer) - 1, "    <section name=\"%s\" number=\"%d\" startTime=\"%.3f\"/>\n", buffer2, sp->beat[ctr]->contained_rs_section_event_instance_number, sp->beat[ctr]->fpos / 1000.0);
				(void) pack_fputs(buffer, fp);
			}
		}
		(void) pack_fputs("  </sections>\n", fp);
	}
	else
	{
		allegro_message("Error:  Default RS sections that were added are missing.  Skipping writing the <sections> tag.");
	}

	//Write events
	for(ctr = 0, numevents = 0; ctr < sp->text_events; ctr++)
	{	//For each event in the chart
		if(sp->text_event[ctr]->flags & EOF_EVENT_FLAG_RS_EVENT)
		{	//If the event is marked as a Rocksmith event
			if(!sp->text_event[ctr]->track || (sp->text_event[ctr]->track  == track))
			{	//If the event applies to the specified track
				numevents++;
			}
		}
	}
	if(numevents)
	{	//If there is at least one Rocksmith event defined in the chart
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <events count=\"%lu\">\n", numevents);
		(void) pack_fputs(buffer, fp);
		for(ctr = 0, numevents = 0; ctr < sp->text_events; ctr++)
		{	//For each event in the chart
			if(sp->text_event[ctr]->flags & EOF_EVENT_FLAG_RS_EVENT)
			{	//If the event is marked as a Rocksmith event
				if(!sp->text_event[ctr]->track || (sp->text_event[ctr]->track  == track))
				{	//If the event applies to the specified track
					expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->text_event[ctr]->text, 256);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
					(void) snprintf(buffer, sizeof(buffer) - 1, "    <event time=\"%.3f\" code=\"%s\"/>\n", sp->beat[sp->text_event[ctr]->beat]->fpos / 1000.0, buffer2);
					(void) pack_fputs(buffer, fp);
				}
			}
		}
		(void) pack_fputs("  </events>\n", fp);
	}
	else
	{	//Otherwise write an empty events tag
		(void) pack_fputs("  <events count=\"0\"/>\n", fp);
	}

	//Write note difficulties
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <levels count=\"%u\">\n", numdifficulties);
	(void) pack_fputs(buffer, fp);
	for(ctr = 0, ctr2 = 0; ctr < 256; ctr++)
	{	//For each of the possible difficulties
		if(eof_track_diff_populated_status[ctr])
		{	//If this difficulty is populated
			unsigned long anchorcount;
			char anchorsgenerated = 0;	//Tracks whether anchors were automatically generated and will need to be deleted after export

			//Count the number of single notes and chords in this difficulty
			for(ctr3 = 0, numsinglenotes = 0, numchords = 0; ctr3 < tp->notes; ctr3++)
			{	//For each note in the track
				if(eof_get_note_type(sp, track, ctr3) == ctr)
				{	//If the note is in this difficulty
					unsigned long lanecount = eof_note_count_rs_lanes(sp, track, ctr3, 1);	//Count the number of non ghosted/muted gems for this note
					if(lanecount == 1)
					{	//If the note has only one gem
						numsinglenotes++;	//Increment counter
					}
					else if(lanecount > 1)
					{	//If the note has multiple gems
						numchords++;	//Increment counter
					}
				}
			}

			//Write single notes
			(void) snprintf(buffer, sizeof(buffer) - 1, "    <level difficulty=\"%lu\">\n", ctr2);
			(void) pack_fputs(buffer, fp);
			ctr2++;	//Increment the populated difficulty level number
			if(numsinglenotes)
			{	//If there's at least one single note in this difficulty
				(void) snprintf(buffer, sizeof(buffer) - 1, "      <notes count=\"%lu\">\n", numsinglenotes);
				(void) pack_fputs(buffer, fp);
				for(ctr3 = 0; ctr3 < tp->notes; ctr3++)
				{	//For each note in the track
					if((eof_get_note_type(sp, track, ctr3) == ctr) && (eof_note_count_rs_lanes(sp, track, ctr3, 1) == 1))
					{	//If this note is in this difficulty and will export as a single note (only one gem has non ghosted/muted status)
						for(stringnum = 0, bitmask = 1; stringnum < tp->numstrings; stringnum++, bitmask <<= 1)
						{	//For each string used in this track
							if((eof_get_note_note(sp, track, ctr3) & bitmask) && ((tp->note[ctr3]->frets[stringnum] & 0x80) == 0) && !(tp->note[ctr3]->ghost & bitmask))
							{	//If this string is used in this note, it is not fret hand muted and it is not ghosted
								unsigned long flags = eof_get_note_flags(sp, track, ctr3);

								if((flags & EOF_PRO_GUITAR_NOTE_FLAG_STRING_MUTE) == 0)
								{	//At this point, it doesn't seem Rocksmith supports string muted notes
									EOF_RS_TECHNIQUES tech;
									unsigned long notepos;
									unsigned long fret;				//The fret number used for this string

									(void) eof_get_rs_techniques(sp, track, ctr3, stringnum, &tech, 1);	//Determine techniques used by this note (run the RS1 check to ensure a pop/slap note isn't allowed to also have bend/slide technique)
									notepos = eof_get_note_pos(sp, track, ctr3);
									fret = tp->note[ctr3]->frets[stringnum] & 0x7F;	//Get the fret value for this string (mask out the muting bit)
									fret += tp->capo;	//Apply the capo position
									if(!eof_pro_guitar_note_lowest_fret(tp, ctr3))
									{	//If this note contains no fretted strings
										if(tech.bend || (tech.slideto >= 0))
										{	//If it is also marked as a bend or slide note, omit these statuses because they're invalid for open notes
											tech.bend = tech.bendstrength_h = tech.bendstrength_q = 0;
											tech.slideto = -1;
											if((*user_warned & 4) == 0)
											{	//If the user wasn't alerted that one or more open notes have these statuses improperly applied
												allegro_message("Warning:  At least one open note is marked with bend or slide status.\nThis is not supported, so these statuses are being omitted for such notes.");
												*user_warned |= 4;
											}
										}
									}
									(void) snprintf(buffer, sizeof(buffer) - 1, "        <note time=\"%.3f\" bend=\"%lu\" fret=\"%lu\" hammerOn=\"%d\" harmonic=\"%d\" hopo=\"%d\" ignore=\"0\" palmMute=\"%d\" pluck=\"%d\" pullOff=\"%d\" slap=\"%d\" slideTo=\"%ld\" string=\"%lu\" sustain=\"%.3f\" tremolo=\"%d\"/>\n", (double)notepos / 1000.0, tech.bendstrength_h, fret, tech.hammeron, tech.harmonic, tech.hopo, tech.palmmute, tech.pop, tech.pulloff, tech.slap, tech.slideto, stringnum, (double)tech.length / 1000.0, tech.tremolo);
									(void) pack_fputs(buffer, fp);
									break;	//Only one note entry is valid for each single note, so break from loop
								}//If the note isn't string muted
							}//If this string is used in this note
						}//For each string used in this track
					}//If this note is in this difficulty and is a single note (and not a chord)
				}//For each note in the track
				(void) pack_fputs("      </notes>\n", fp);
			}//If there's at least one single note in this difficulty
			else
			{	//There are no single notes in this difficulty, write an empty notes tag
				(void) pack_fputs("      <notes count=\"0\"/>\n", fp);
			}

			//Write chords
			if(numchords)
			{	//If there's at least one chord in this difficulty
				unsigned long chordid;
				char *upstrum = "up";
				char *downstrum = "down";
				char *direction;	//Will point to either upstrum or downstrum as appropriate
				double notepos;
				char highdensity;		//Any chord within the threshold proximity of an identical chord has the highDensity boolean property set to true

				(void) snprintf(buffer, sizeof(buffer) - 1, "      <chords count=\"%lu\">\n", numchords);
				(void) pack_fputs(buffer, fp);
				for(ctr3 = 0; ctr3 < tp->notes; ctr3++)
				{	//For each note in the track
					if((eof_get_note_type(sp, track, ctr3) == ctr) && (eof_note_count_rs_lanes(sp, track, ctr3, 1) > 1))
					{	//If this note is in this difficulty and will export as a chord (at least two non ghosted/muted gems)
						for(ctr4 = 0; ctr4 < chordlistsize; ctr4++)
						{	//For each of the entries in the unique chord list
							if(!eof_note_compare_simple(sp, track, ctr3, chordlist[ctr4]))
							{	//If this note matches a chord list entry
								chordid = ctr4;	//Store the chord list entry number
								break;
							}
						}
						if(ctr4 >= chordlistsize)
						{	//If the chord couldn't be found
							allegro_message("Error:  Couldn't match chord with chord template while exporting chords.  Aborting Rocksmith 1 export.");
							eof_log("Error:  Couldn't match chord with chord template while exporting chords.  Aborting Rocksmith 1 export.", 1);
							if(chordlist)
							{	//If the chord list was built
								free(chordlist);
							}
							return 0;	//Return error
						}
						if(tp->note[ctr3]->flags & EOF_PRO_GUITAR_NOTE_FLAG_UP_STRUM)
						{	//If this note explicitly strums up
							direction = upstrum;	//Set the direction string to match
						}
						else
						{	//Otherwise the direction defaults to down
							direction = downstrum;
						}
						highdensity = eof_note_has_high_chord_density(sp, track, ctr3, 1);	//Determine whether the chord will export with high density
						notepos = (double)tp->note[ctr3]->pos / 1000.0;
						(void) snprintf(buffer, sizeof(buffer) - 1, "        <chord time=\"%.3f\" chordId=\"%lu\" highDensity=\"%d\" ignore=\"0\" strum=\"%s\"/>\n", notepos, chordid, highdensity, direction);
						(void) pack_fputs(buffer, fp);
					}//If this note is in this difficulty and is a chord
				}//For each note in the track
				(void) pack_fputs("      </chords>\n", fp);
			}
			else
			{	//There are no chords in this difficulty, write an empty chords tag
				(void) pack_fputs("      <chords count=\"0\"/>\n", fp);
			}

			//Write other stuff
			(void) pack_fputs("      <fretHandMutes count=\"0\"/>\n", fp);

			//Write anchors (fret hand positions)
			for(ctr3 = 0, anchorcount = 0; ctr3 < tp->handpositions; ctr3++)
			{	//For each hand position defined in the track
				if(tp->handposition[ctr3].difficulty == ctr)
				{	//If the hand position is in this difficulty
					anchorcount++;
				}
			}
			if(!anchorcount)
			{	//If there are no anchors in this track difficulty, automatically generate them
				if((*user_warned & 1) == 0)
				{	//If the user wasn't alerted that one or more track difficulties have no fret hand positions defined
					allegro_message("Warning:  At least one track difficulty has no fret hand positions defined.  They will be created automatically.");
					*user_warned |= 1;
				}
				eof_fret_hand_position_list_dialog_undo_made = 1;	//Ensure no undo state is written during export
				eof_generate_efficient_hand_positions(sp, track, ctr, 0, 0);	//Generate the fret hand positions for the track difficulty being currently written (use a static fret range tolerance of 4 for all frets)
				anchorsgenerated = 1;
			}
			for(ctr3 = 0, anchorcount = 0; ctr3 < tp->handpositions; ctr3++)	//Re-count the hand positions
			{	//For each hand position defined in the track
				if(tp->handposition[ctr3].difficulty == ctr)
				{	//If the hand position is in this difficulty
					anchorcount++;
				}
			}
			if(anchorcount)
			{	//If there's at least one anchor in this difficulty
				(void) snprintf(buffer, sizeof(buffer) - 1, "      <anchors count=\"%lu\">\n", anchorcount);
				(void) pack_fputs(buffer, fp);
				for(ctr3 = 0, anchorcount = 0; ctr3 < tp->handpositions; ctr3++)
				{	//For each hand position defined in the track
					if(tp->handposition[ctr3].difficulty == ctr)
					{	//If the hand position is in this difficulty
						unsigned long fret = tp->handposition[ctr3].end_pos + tp->capo;	////Apply the capo position
						(void) snprintf(buffer, sizeof(buffer) - 1, "        <anchor time=\"%.3f\" fret=\"%lu\"/>\n", (double)tp->handposition[ctr3].start_pos / 1000.0, fret);
						(void) pack_fputs(buffer, fp);
					}
				}
				(void) pack_fputs("      </anchors>\n", fp);
			}
			else
			{	//There are no anchors in this difficulty, write an empty anchors tag
				(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "Error:  Failed to automatically generate fret hand positions for level %lu of\n\"%s\" during MIDI export.", ctr2, fn);
				eof_log(eof_log_string, 1);
				allegro_message(eof_log_string);
				(void) pack_fputs("      <anchors count=\"0\"/>\n", fp);
			}
			if(anchorsgenerated)
			{	//If anchors were automatically generated for this track difficulty, remove them now
				for(ctr3 = tp->handpositions; ctr3 > 0; ctr3--)
				{	//For each hand position defined in the track, in reverse order
					if(tp->handposition[ctr3 - 1].difficulty == ctr)
					{	//If the hand position is in this difficulty
						eof_pro_guitar_track_delete_hand_position(tp, ctr3 - 1);	//Delete the hand position
					}
				}
			}

			//Write hand shapes
			//Count the number of hand shapes to write
			handshapectr = 0;
			for(ctr3 = 0; ctr3 < tp->notes; ctr3++)
			{	//For each note in the track
				if((eof_get_note_type(sp, track, ctr3) == ctr) && ((eof_note_count_rs_lanes(sp, track, ctr3, 1) > 1) || eof_is_partially_ghosted(sp, track, ctr3)))
				{	//If this note is in this difficulty and will export as a chord (at least two non ghosted/muted gems) or an arpeggio handshape
					unsigned long chord = ctr3;	//Store a copy of this note number because ctr3 will be manipulated below

					//Find this chord's ID
					for(ctr4 = 0; ctr4 < chordlistsize; ctr4++)
					{	//For each of the entries in the unique chord list
						if(!eof_note_compare_simple(sp, track, ctr3, chordlist[ctr4]))
						{	//If this note matches a chord list entry
							chordid = ctr4;	//Store the chord list entry number
							break;
						}
					}
					if(ctr4 >= chordlistsize)
					{	//If the chord couldn't be found
						allegro_message("Error:  Couldn't match chord with chord template while counting handshapes.  Aborting Rocksmith 1 export.");
						eof_log("Error:  Couldn't match chord with chord template while counting handshapes.  Aborting Rocksmith 1 export.", 1);
						if(chordlist)
						{	//If the chord list was built
							free(chordlist);
						}
						return 0;	//Return error
					}
					handshapestart = eof_get_note_pos(sp, track, ctr3);	//Store this chord's start position

					//If this chord is at the beginning of an arpeggio phrase, skip the rest of the notes in that phrase
					for(ctr5 = 0; ctr5 < tp->arpeggios; ctr5++)
					{	//For each arpeggio phrase in the track
						if(((tp->note[ctr3]->pos + 10 >= tp->arpeggio[ctr5].start_pos) && (tp->note[ctr3]->pos <= tp->arpeggio[ctr5].start_pos + 10)) && (tp->note[ctr3]->type == tp->arpeggio[ctr5].difficulty))
						{	//If this chord's start position is within 10ms of an arpeggio phrase in this track difficulty
							while(1)
							{
								nextnote = eof_fixup_next_note(sp, track, ctr3);
								if((nextnote >= 0) && (tp->note[nextnote]->pos <= tp->arpeggio[ctr5].end_pos))
								{	//If there is another note and it is in the same arpeggio phrase
									ctr3 = nextnote;	//Iterate to that note, and check subsequent notes to see if they are also in the phrase
								}
								else
								{	//The next note (if any) is not in the arpeggio phrase
									break;	//Break from while loop
								}
							}
							break;	//Break from for loop
						}
					}

					//Examine subsequent notes to see if they match this chord
					while(1)
					{
						nextnote = eof_fixup_next_note(sp, track, ctr3);
						if((nextnote >= 0) && !eof_note_compare_simple(sp, track, chord, nextnote) && !eof_is_partially_ghosted(sp, track, nextnote))
						{	//If there is another note, it matches this chord and it is not partially ghosted (an arpeggio)
							ctr3 = nextnote;	//Iterate to that note, and check subsequent notes to see if they match
						}
						else
						{	//The next note (if any) is not a repeat of this note
							handshapeend = eof_get_note_pos(sp, track, ctr3) + eof_get_note_length(sp, track, ctr3);	//End the hand shape at the end of this chord
							break;	//Break from while loop
						}
					}

					handshapectr++;	//One more hand shape has been counted
				}//If this note is in this difficulty and will export as a chord (at least two non ghosted/muted gems)
			}//For each note in the track

			if(handshapectr)
			{	//If there was at least one hand shape to write
				//Write the hand shapes
				(void) snprintf(buffer, sizeof(buffer) - 1, "      <handShapes count=\"%lu\">\n", handshapectr);
				(void) pack_fputs(buffer, fp);
				for(ctr3 = 0; ctr3 < tp->notes; ctr3++)
				{	//For each note in the track
					if((eof_get_note_type(sp, track, ctr3) == ctr) && ((eof_note_count_rs_lanes(sp, track, ctr3, 1) > 1) || eof_is_partially_ghosted(sp, track, ctr3)))
					{	//If this note is in this difficulty and will export as a chord (at least two non ghosted/muted gems) or an arpeggio handshape
						unsigned long chord = ctr3;	//Store a copy of this note number because ctr3 will be manipulated below

						//Find this chord's ID
						for(ctr4 = 0; ctr4 < chordlistsize; ctr4++)
						{	//For each of the entries in the unique chord list
							if(!eof_note_compare_simple(sp, track, ctr3, chordlist[ctr4]))
							{	//If this note matches a chord list entry
								chordid = ctr4;	//Store the chord list entry number
								break;
							}
						}
						if(ctr4 >= chordlistsize)
						{	//If the chord couldn't be found
							allegro_message("Error:  Couldn't match chord with chord template while writing handshapes.  Aborting Rocksmith 1 export.");
							eof_log("Error:  Couldn't match chord with chord template while writing handshapes.  Aborting Rocksmith 1 export.", 1);
							if(chordlist)
							{	//If the chord list was built
								free(chordlist);
							}
							return 0;	//Return error
						}
						handshapestart = eof_get_note_pos(sp, track, ctr3);	//Store this chord's start position (in seconds)

						//If this chord is at the beginning of an arpeggio phrase, skip the rest of the notes in that phrase
						for(ctr5 = 0; ctr5 < tp->arpeggios; ctr5++)
						{	//For each arpeggio phrase in the track
							if(((tp->note[ctr3]->pos + 10 >= tp->arpeggio[ctr5].start_pos) && (tp->note[ctr3]->pos <= tp->arpeggio[ctr5].start_pos + 10)) && (tp->note[ctr3]->type == tp->arpeggio[ctr5].difficulty))
							{	//If this chord's start position is within 10ms of an arpeggio phrase in this track difficulty
								while(1)
								{
									nextnote = eof_fixup_next_note(sp, track, ctr3);
									if((nextnote >= 0) && (tp->note[nextnote]->pos <= tp->arpeggio[ctr5].end_pos))
									{	//If there is another note and it is in the same arpeggio phrase
										ctr3 = nextnote;	//Iterate to that note, and check subsequent notes to see if they are also in the phrase
									}
									else
									{	//The next note (if any) is not in the arpeggio phrase
										break;	//Break from while loop
									}
								}
								break;	//Break from for loop
							}
						}

						//Examine subsequent notes to see if they match this chord
						while(1)
						{
							nextnote = eof_fixup_next_note(sp, track, ctr3);
							if((nextnote >= 0) && !eof_note_compare_simple(sp, track, chord, nextnote) && !eof_is_partially_ghosted(sp, track, nextnote))
							{	//If there is another note, it matches this chord and it is not partially ghosted (an arpeggio)
								ctr3 = nextnote;	//Iterate to that note, and check subsequent notes to see if they match
							}
							else
							{	//The next note (if any) is not a repeat of this note
								handshapeend = eof_get_note_pos(sp, track, ctr3) + eof_get_note_length(sp, track, ctr3);	//End the hand shape at the end of this chord

								if((handshapeend - handshapestart < 56) && (handshapestart + 56 < eof_get_note_pos(sp, track, nextnote)))
								{	//If the hand shape would be shorter than 56ms, and the next note is further than 56ms away
									handshapeend = eof_get_note_pos(sp, track, ctr3) + 56;	//Pad the hand shape to 56ms
								}
								break;	//Break from while loop
							}
						}

						//Write this hand shape
						(void) snprintf(buffer, sizeof(buffer) - 1, "        <handShape chordId=\"%lu\" endTime=\"%.3f\" startTime=\"%.3f\"/>\n", chordid, (double)handshapeend / 1000.0, (double)handshapestart / 1000.0);
						(void) pack_fputs(buffer, fp);
					}//If this note is in this difficulty and will export as a chord (at least two non ghosted/muted gems)
				}//For each note in the track
				(void) pack_fputs("      </handShapes>\n", fp);
			}
			else
			{	//There are no chords in this difficulty, write an empty hand shape tag
				(void) pack_fputs("      <handShapes count=\"0\"/>\n", fp);
			}

			//Write closing level tag
			(void) pack_fputs("    </level>\n", fp);
		}//If this difficulty is populated
	}//For each of the available difficulties
	(void) pack_fputs("  </levels>\n", fp);
	(void) pack_fputs("</song>\n", fp);
	(void) pack_fclose(fp);

	//Cleanup
	if(chordlist)
	{	//If the chord list was built
		free(chordlist);
	}
	//Remove all temporary text events that were added
	for(ctr = sp->text_events; ctr > 0; ctr--)
	{	//For each text event (in reverse order)
		if(sp->text_event[ctr - 1]->is_temporary)
		{	//If this text event has been marked as temporary
			eof_song_delete_text_event(sp, ctr - 1);	//Delete it
		}
	}
	eof_sort_events(sp);	//Re-sort events
	//Remove all temporary notes that were added
	for(ctr = tp->notes; ctr > 0; ctr--)
	{	//For each note in the track, in reverse order
		if(tp->note[ctr - 1]->tflags & EOF_NOTE_TFLAG_TEMP)
		{	//If this is a temporary note that was added for chord technique notation
			eof_track_delete_note(sp, track, ctr - 1);	//Delete it
		}
	}
	eof_track_sort_notes(sp, track);	//Re-sort the notes

	return 1;	//Return success
}

int eof_export_rocksmith_2_track(EOF_SONG * sp, char * fn, unsigned long track, char *user_warned)
{
	PACKFILE * fp;
	char buffer[600] = {0}, buffer2[512] = {0};
	time_t seconds;		//Will store the current time in seconds
	struct tm *caltime;	//Will store the current time in calendar format
	unsigned long ctr, ctr2, ctr3, ctr4, ctr5, numsections, stringnum, bitmask, numsinglenotes, numchords, *chordlist, chordlistsize, *sectionlist, sectionlistsize, xml_end, numevents = 0;
	EOF_PRO_GUITAR_TRACK *tp;
	char *arrangement_name;	//This will point to the track's native name unless it has an alternate name defined
	unsigned numdifficulties;
	unsigned char bre_populated = 0;
	unsigned long phraseid;
	unsigned beatspermeasure = 4, beatcounter = 0;
	long displayedmeasure, measurenum = 0;
	long startbeat;	//This will indicate the first beat containing a note in the track
	long endbeat;	//This will indicate the first beat after the exported track's last note
	char standard[] = {0,0,0,0,0,0};
	char standardbass[] = {0,0,0,0};
	char eb[] = {-1,-1,-1,-1,-1,-1};
	char dropd[] = {-2,0,0,0,0,0};
	char openg[] = {-2,-2,0,0,0,-2};
	char *tuning;
	char isebtuning = 1;	//Will track whether all strings are tuned to -1
	char notename[EOF_NAME_LENGTH+1];	//String large enough to hold any chord name supported by EOF
	int scale, chord, isslash, bassnote;	//Used for power chord detection
	int standard_tuning = 0, non_standard_chords = 0, barre_chords = 0, power_chords = 0, notenum, dropd_tuning = 1, dropd_power_chords = 0, open_chords = 0, double_stops = 0, palm_mutes = 0, harmonics = 0, hopo = 0, tremolo = 0, slides = 0, bends = 0, tapping = 0, vibrato = 0, slappop = 0, octaves = 0, fifths_and_octaves = 0, sustains = 0, pinch= 0;	//Used for technique detection
	int is_lead = 0, is_rhythm = 0, is_bass = 0;	//Is set to nonzero if the specified track is to be considered any of these arrangement types
	char end_phrase_found = 0;	//Will track if there was a manually defined END phrase
	unsigned long chordid, handshapectr;
	unsigned long handshapestart, handshapeend;
	long nextnote;
	unsigned long originalbeatcount;	//If beats are padded to reach the beginning of the next measure (for DDC), this will track the project's original number of beats
	EOF_RS_TECHNIQUES tech;

	eof_log("eof_export_rocksmith_2_track() entered", 1);

	if(!sp || !fn || !sp->beats || (track >= sp->tracks) || (sp->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT) || !sp->track[track]->name || !user_warned)
	{
		eof_log("\tError saving:  Invalid parameters", 1);
		return 0;	//Return failure
	}

	tp = sp->pro_guitar_track[sp->track[track]->tracknum];
	if(eof_get_highest_fret(sp, track, 0) > 22)
	{	//If the track being exported uses any frets higher than 22
		if((*user_warned & 2) == 0)
		{	//If the user wasn't alerted about this issue yet
			allegro_message("Warning:  At least one track (\"%s\") uses a fret higher than 22.  This will cause Rocksmith to crash.", sp->track[track]->name);
			*user_warned |= 2;
		}
	}
	for(ctr = 0; ctr < eof_get_track_size(sp, track); ctr++)
	{	//For each note in the track
		unsigned char slideend;
		unsigned long flags = eof_get_note_flags(sp, track, ctr);

		if((flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP) || (flags & EOF_PRO_GUITAR_NOTE_FLAG_UNPITCH_SLIDE))
		{	//If the note slides up or unpitched slides up
			slideend = tp->note[ctr]->slideend + tp->capo;	//Obtain the end position of the slide, take the capo position into account
			if((flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP) && !(flags & EOF_PRO_GUITAR_NOTE_FLAG_RS_NOTATION))
			{	//If this pitched slide's end position is not defined
				slideend = eof_get_highest_fret_value(sp, track, ctr) + 1;	//Assume a 1 fret slide
			}
			if((slideend >= 22) || ((tp->note[ctr]->unpitchend + tp->capo) >= 22))
			{	//If either type of slide goes to or above fret 22 (after taking the capo position into account)
				if((*user_warned & 8) == 0)
				{	//If the user wasn't alerted about this issue yet
					eof_seek_and_render_position(track, tp->note[ctr]->type, tp->note[ctr]->pos);	//Show the offending note
					allegro_message("Warning:  At least one note slides to or above fret 22.  This could cause Rocksmith 2 to crash.");
					*user_warned |= 8;
				}
				break;
			}
		}
	}

	//Count the number of populated difficulties in the track
	(void) eof_detect_difficulties(sp, track);	//Update eof_track_diff_populated_status[] to reflect all populated difficulties for this track
	if((sp->track[track]->flags & EOF_TRACK_FLAG_UNLIMITED_DIFFS) == 0)
	{	//If the track is using the traditional 5 difficulty system
		if(eof_track_diff_populated_status[4])
		{	//If the BRE difficulty is populated
			bre_populated = 1;	//Track that it was
		}
		eof_track_diff_populated_status[4] = 0;	//Ensure that the BRE difficulty is not exported
	}
	for(ctr = 0, numdifficulties = 0; ctr < 256; ctr++)
	{	//For each possible difficulty
		if(eof_track_diff_populated_status[ctr])
		{	//If this difficulty is populated
			numdifficulties++;	//Increment this counter
		}
	}
	if(!numdifficulties)
	{
		(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "Cannot export track \"%s\"in Rocksmith format, it has no populated difficulties", sp->track[track]->name);
		eof_log(eof_log_string, 1);
		if(bre_populated)
		{	//If the BRE difficulty was the only one populated, warn that it is being omitted
			(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "Warning:  Track \"%s\" only has notes in the BRE difficulty.\nThese are not exported in Rocksmith format unless you remove the difficulty limit (Song>Rocksmith>Remove difficulty limit).", sp->track[track]->name);
			allegro_message(eof_log_string);
			eof_log(eof_log_string, 1);
		}
		return 0;	//Return failure
	}

	//Update target file name and open it for writing
	if((sp->track[track]->flags & EOF_TRACK_FLAG_ALT_NAME) && (sp->track[track]->altname[0] != '\0'))
	{	//If the track has an alternate name
		arrangement_name = sp->track[track]->altname;
	}
	else
	{	//Otherwise use the track's native name
		arrangement_name = sp->track[track]->name;
	}
	(void) snprintf(buffer, 600, "%s_RS2.xml", arrangement_name);
	(void) replace_filename(fn, fn, buffer, 1024);
	fp = pack_fopen(fn, "w");
	if(!fp)
	{
		eof_log("\tError saving:  Cannot open file for writing", 1);
		return 0;	//Return failure
	}

	//Update the track's arrangement name
	if(tp->arrangement)
	{	//If the track's arrangement type has been defined
		arrangement_name = eof_rs_arrangement_names[tp->arrangement];	//Use the corresponding string in the XML file
	}

	//Get the smaller of the chart length and the music length, this will be used to write the songlength tag
	xml_end = eof_music_length;
	if(eof_silence_loaded || (eof_chart_length < eof_music_length))
	{	//If the chart length is shorter than the music length, or there is no chart audio loaded
		xml_end = eof_chart_length;	//Use the chart's length instead
	}

	//Write the beginning of the XML file
	(void) pack_fputs("<?xml version='1.0' encoding='UTF-8'?>\n", fp);
	(void) pack_fputs("<song version=\"7\">\n", fp);
	(void) pack_fputs("<!-- " EOF_VERSION_STRING " -->\n", fp);	//Write EOF's version in an XML comment
	expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->tags->title, 64);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <title>%s</title>\n", buffer2);
	(void) pack_fputs(buffer, fp);
	expand_xml_text(buffer2, sizeof(buffer2) - 1, arrangement_name, 32);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <arrangement>%s</arrangement>\n", buffer2);
	(void) pack_fputs(buffer, fp);
	(void) pack_fputs("  <part>1</part>\n", fp);
	(void) pack_fputs("  <offset>0.000</offset>\n", fp);
	(void) pack_fputs("  <centOffset>0</centOffset>\n", fp);
	eof_truncate_chart(sp);	//Update the chart length
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <songLength>%.3f</songLength>\n", (double)(xml_end - 1) / 1000.0);	//Make sure the song length is not longer than the actual audio, or the chart won't reach an end in-game
	(void) pack_fputs(buffer, fp);
	seconds = time(NULL);
	caltime = localtime(&seconds);
	if(caltime)
	{	//If the calendar time could be determined
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <lastConversionDateTime>%d-%d-%d %d:%02d</lastConversionDateTime>\n", caltime->tm_mon + 1, caltime->tm_mday, caltime->tm_year % 100, caltime->tm_hour % 12, caltime->tm_min);
	}
	else
	{
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <lastConversionDateTime>UNKNOWN</lastConversionDateTime>\n");
	}
	(void) pack_fputs(buffer, fp);

	//Write additional tags to pass song information to the Rocksmith toolkit
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <startBeat>%.3f</startBeat>\n", sp->beat[0]->fpos / 1000.0);	//The position of the first beat
	(void) pack_fputs(buffer, fp);
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <averageTempo>%.3f</averageTempo>\n", 60000.0 / ((sp->beat[sp->beats - 1]->fpos - sp->beat[0]->fpos) / sp->beats));	//The average tempo (60000ms / the average beat length in ms)
	(void) pack_fputs(buffer, fp);
	tuning = tp->tuning;	//By default, use the track's original tuning array
	for(ctr = 0; ctr < 6; ctr++)
	{	//For each string EOF supports
		if(ctr >= tp->numstrings)
		{	//If the track doesn't use this string
			tp->tuning[ctr] = 0;	//Ensure the tuning is cleared accordingly
		}
	}
	for(ctr = 0; ctr < tp->numstrings; ctr++)
	{	//For each string in this track
		if(tp->tuning[ctr] != -1)
		{	//If this string isn't tuned a half step down
			isebtuning = 0;
			break;
		}
	}
	is_bass = eof_track_is_bass_arrangement(tp, track);
	if(tp->arrangement == 2)
	{	//Rhythm arrangement
		is_rhythm = 1;
	}
	else if(tp->arrangement == 3)
	{	//Lead arrangement
		is_lead = 1;
	}
	if(isebtuning && !(is_bass && (tp->numstrings > 4)))
	{	//If all strings were tuned down a half step (except for bass tracks with more than 4 strings, since in those cases, the lowest string is not tuned to E)
		tuning = eb;	//Remap 4 or 5 string Eb tuning as {-1,-1,-1,-1,-1,-1}
	}
	if(memcmp(tuning, standard, 6) && memcmp(tuning, standardbass, 4) && memcmp(tuning, eb, 6) && memcmp(tuning, dropd, 6) && memcmp(tuning, openg, 6))
	{	//If the track's tuning doesn't match any supported by Rocksmith
		allegro_message("Warning:  This track (%s) uses a tuning that isn't one known to be supported in Rocksmith.\nTuning and note recognition may not work as expected in-game", sp->track[track]->name);
		(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "Warning:  This track (%s) uses a tuning that isn't known to be supported in Rocksmith.  Tuning and note recognition may not work as expected in-game", sp->track[track]->name);
		eof_log(eof_log_string, 1);
	}
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <tuning string0=\"%d\" string1=\"%d\" string2=\"%d\" string3=\"%d\" string4=\"%d\" string5=\"%d\" />\n", tuning[0], tuning[1], tuning[2], tuning[3], tuning[4], tuning[5]);
	(void) pack_fputs(buffer, fp);
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <capo>%d</capo>\n", tp->capo);
	(void) pack_fputs(buffer, fp);
	expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->tags->artist, 256);	//Replace any special characters in the artist song property with escape sequences if necessary
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <artistName>%s</artistName>\n", buffer2);
	(void) pack_fputs(buffer, fp);
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <artistNameSort>%s</artistNameSort>\n", buffer2);
	(void) pack_fputs(buffer, fp);
	expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->tags->album, 256);	//Replace any special characters in the album song property with escape sequences if necessary
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <albumName>%s</albumName>\n", buffer2);
	(void) pack_fputs(buffer, fp);
	expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->tags->year, 32);	//Replace any special characters in the year song property with escape sequences if necessary
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <albumYear>%s</albumYear>\n", buffer2);
	(void) pack_fputs(buffer, fp);
	(void) pack_fputs("  <crowdSpeed>1</crowdSpeed>\n", fp);

	//Determine arrangement properties
	if(!memcmp(tuning, standard, 6))
	{	//All unused strings had their tuning set to 0, so if all bytes of this array are 0, the track is in standard tuning
		standard_tuning = 1;
	}
	notenum = eof_lookup_tuned_note(tp, track, 0, tp->tuning[0]);	//Look up the open note the lowest string plays
	notenum %= 12;	//Ensure the value is in the range of [0,11]
	if(notenum == 5)
	{	//If the lowest string is tuned to D
		for(ctr = 1; ctr < 6; ctr++)
		{	//For the other 5 strings
			if(tp->tuning[ctr] != 0)
			{	//If the string is not in standard tuning
				dropd_tuning = 0;
			}
		}
	}
	else
	{	//The lowest string is not tuned to D
		dropd_tuning = 0;
	}
	eof_determine_phrase_status(sp, track);	//Update the tremolo status of each note
	for(ctr = 0; ctr < tp->notes; ctr++)
	{	//For each note in the track
		if(eof_note_count_rs_lanes(sp, track, ctr, 2) > 1)
		{	//If the note will export as a chord (more than one non ghosted/muted gem)
			if(!non_standard_chords && !eof_build_note_name(sp, track, ctr, notename))
			{	//If the chord has no defined or detected name (only if this condition hasn't been found already)
				non_standard_chords = 1;
			}
			if(!barre_chords && eof_pro_guitar_note_is_barre_chord(tp, ctr))
			{	//If the chord is a barre chord (only if this condition hasn't been found already)
				barre_chords = 1;
			}
			if(!power_chords && eof_lookup_chord(tp, track, ctr, &scale, &chord, &isslash, &bassnote, 0, 0))
			{	//If the chord lookup found a match (only if this condition hasn't been found already)
				if(chord == 27)
				{	//27 is the index of the power chord formula in eof_chord_names[]
					power_chords = 1;
					if(dropd_tuning)
					{	//If this track is in drop d tuning
						dropd_power_chords = 1;
					}
				}
			}
			if(!open_chords)
			{	//Only if no open chords have been found already
				for(ctr2 = 0, bitmask = 1; ctr2 < 6; ctr2++, bitmask <<= 1)
				{	//For each of the 6 supported strings
					if((tp->note[ctr]->note & bitmask) && (tp->note[ctr]->frets[ctr2] == 0))
					{	//If this string is used and played open
						open_chords = 1;
					}
				}
			}
			if(!double_stops && eof_pro_guitar_note_is_double_stop(tp, ctr))
			{	//If the chord is a double stop (only if this condition hasn't been found already)
				double_stops = 1;
			}
			if(is_bass)
			{	//If the arrangement being exported is bass
				int thisnote, lastnote = -1, failed = 0;
				for(ctr2 = 0, bitmask = 1; ctr2 < 6; ctr2++, bitmask <<= 1)
				{	//For each of the 6 supported strings
					if((tp->note[ctr]->note & bitmask) && !(tp->note[ctr]->frets[ctr2] & 0x80))
					{	//If this string is played and not string muted
						thisnote = eof_lookup_played_note(tp, track, ctr2, tp->note[ctr]->frets[ctr2]);	//Determine what note is being played
						if((lastnote >= 0) && (lastnote != thisnote))
						{	//If this string's played note doesn't match the note played by previous strings
							failed = 1;
							break;
						}
						lastnote = thisnote;
					}
				}
				if(!failed)
				{	//If non muted strings in the chord played the same note
					octaves = 1;
				}
			}
		}//If the note is a chord (more than one non ghosted gem)
		else
		{	//If it will export as a single note
			if(tp->note[ctr]->length > 1)
			{	//If the note is a sustain
				sustains = 1;
			}
		}
		if(tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_PALM_MUTE)
		{	//If the note is palm muted
			palm_mutes = 1;
		}
		if(tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_HARMONIC)
		{	//If the note is a harmonic
			harmonics = 1;
		}
		if((tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_HO) || (tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_PO))
		{	//If the note is a hammer on or pull off
			hopo = 1;
		}
		if(tp->note[ctr]->flags & EOF_NOTE_FLAG_IS_TREMOLO)
		{	//If the note is played with tremolo
			tremolo = 1;
		}
		if((tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP) || (tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN))
		{	//If the note slides up or down
			slides = 1;
		}
		if(tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_BEND)
		{	//If the note is bent
			bends = 1;
		}
		if(tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_TAP)
		{	//If the note is tapped
			tapping = 1;
		}
		if(tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_VIBRATO)
		{	//If the note is played with vibrato
			vibrato = 1;
		}
		if((tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_SLAP) || (tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_POP))
		{	//If the note is played by slapping or popping
			slappop = 1;
		}
		if(tp->note[ctr]->flags & EOF_PRO_GUITAR_NOTE_FLAG_P_HARMONIC)
		{	//If the note is played with vibrato
			pinch = 1;
		}
	}//For each note in the track
	if(is_bass)
	{	//If the arrangement being exported is bass
		if(double_stops || octaves)
		{	//If either of these techniques were detected
			fifths_and_octaves = 1;
		}
		double_stops = 0;
	}
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <arrangementProperties represent=\"1\" bonusArr=\"0\" standardTuning=\"%d\" nonStandardChords=\"%d\" barreChords=\"%d\" powerChords=\"%d\" dropDPower=\"%d\" openChords=\"%d\" fingerPicking=\"0\" pickDirection=\"0\" doubleStops=\"%d\" palmMutes=\"%d\" harmonics=\"%d\" pinchHarmonics=\"%d\" hopo=\"%d\" tremolo=\"%d\" slides=\"%d\" unpitchedSlides=\"0\" bends=\"%d\" tapping=\"%d\" vibrato=\"%d\" fretHandMutes=\"0\" slapPop=\"%d\" twoFingerPicking=\"0\" fifthsAndOctaves=\"%d\" syncopation=\"0\" bassPick=\"0\" sustain=\"%d\" pathLead=\"%d\" pathRhythm=\"%d\" pathBass=\"%d\" />\n", standard_tuning, non_standard_chords, barre_chords, power_chords, dropd_power_chords, open_chords, double_stops, palm_mutes, harmonics, pinch, hopo, tremolo, slides, bends, tapping, vibrato, slappop, fifths_and_octaves, sustains, is_lead, is_rhythm, is_bass);
	(void) pack_fputs(buffer, fp);

	//Check if any RS phrases or sections need to be added
	startbeat = eof_get_beat(sp, tp->note[0]->pos);	//Find the beat containing the track's first note
	if(startbeat < 0)
	{	//If the beat couldn't be found
		startbeat = 0;	//Set this to the first beat
	}
	endbeat = eof_get_beat(sp, tp->note[tp->notes - 1]->pos + tp->note[tp->notes - 1]->length);	//Find the beat containing the end of the track's last note
	if((endbeat < 0) || (endbeat + 1 >= sp->beats))
	{	//If the beat couldn't be found, or the extreme last beat in the track has the last note
		endbeat = sp->beats - 1;	//Set this to the last beat
	}
	else
	{
		endbeat++;	//Otherwise set it to the first beat that follows the end of the last note
	}
	eof_process_beat_statistics(sp, track);	//Cache section name information into the beat structures (from the perspective of the specified track)
	if(!eof_song_contains_event(sp, "COUNT", track, EOF_EVENT_FLAG_RS_PHRASE, 1) && !eof_song_contains_event(sp, "COUNT", 0, EOF_EVENT_FLAG_RS_PHRASE, 1))
	{	//If the user did not define a COUNT phrase that applies to either the track being exported or all tracks
		if(sp->beat[0]->contained_section_event >= 0)
		{	//If there is already a phrase defined on the first beat
			allegro_message("Warning:  There is no COUNT phrase, but the first beat marker already has a phrase.\nYou should move that phrase because only one phrase per beat is exported.");
		}
		eof_log("\t! Adding missing COUNT phrase", 1);
		(void) eof_song_add_text_event(sp, 0, "COUNT", 0, EOF_EVENT_FLAG_RS_PHRASE, 1);	//Add it as a temporary event at the first beat
	}
	for(ctr = 0; ctr < sp->beats; ctr++)
	{	//For each beat
		if((sp->beat[ctr]->contained_section_event >= 0) && !ustricmp(sp->text_event[sp->beat[ctr]->contained_section_event]->text, "END"))
		{	//If this beat contains an "END" RS phrase
			for(ctr2 = ctr + 1; ctr2 < sp->beats; ctr2++)
			{	//For each remaining beat
				if((sp->beat[ctr2]->contained_section_event >= 0) || (sp->beat[ctr2]->contained_rs_section_event >= 0))
				{	//If the beat contains an RS phrase or RS section
					eof_2d_render_top_option = 36;	//Change the user preference to display RS phrases and sections
					eof_selected_beat = ctr;		//Select it
					eof_seek_and_render_position(track, eof_note_type, sp->beat[ctr]->pos);	//Show the offending END phrase
					allegro_message("Warning:  Beat #%lu contains an END phrase, but there's at least one more phrase or section after it.\nThis will cause dynamic difficulty and/or riff repeater to not work correctly.", ctr);
					break;
				}
			}
			end_phrase_found = 1;
			break;
		}
	}
	if(!end_phrase_found)
	{	//If the user did not define a END phrase
		if(sp->beat[endbeat]->contained_section_event >= 0)
		{	//If there is already a phrase defined on the beat following the last note
			eof_2d_render_top_option = 36;	//Change the user preference to display RS phrases and sections
			eof_selected_beat = endbeat;		//Select it
			eof_seek_and_render_position(track, eof_note_type, sp->beat[endbeat]->pos);	//Show where the END phrase should go
			allegro_message("Warning:  There is no END phrase, but the beat marker after the last note in \"%s\" already has a phrase.\nYou should move that phrase because only one phrase per beat is exported.", sp->track[track]->name);
		}
		eof_log("\t! Adding missing END phrase", 1);
		(void) eof_song_add_text_event(sp, endbeat, "END", 0, EOF_EVENT_FLAG_RS_PHRASE, 1);	//Add it as a temporary event at the last beat
	}
	eof_sort_events(sp);	//Re-sort events
	eof_process_beat_statistics(sp, track);	//Cache section name information into the beat structures (from the perspective of the specified track)
	if(!eof_song_contains_event(sp, "intro", track, EOF_EVENT_FLAG_RS_SECTION, 1) && !eof_song_contains_event(sp, "intro", 0, EOF_EVENT_FLAG_RS_SECTION, 1))
	{	//If the user did not define an intro RS section that applies to either the track being exported or all tracks
		if(sp->beat[startbeat]->contained_rs_section_event >= 0)
		{	//If there is already a RS section defined on the first beat containing a note
			allegro_message("Warning:  There is no intro RS section, but the beat marker before the first note already has a section.\nYou should move that section because only one section per beat is exported.");
		}
		eof_log("\t! Adding missing intro RS section", 1);
		(void) eof_song_add_text_event(sp, startbeat, "intro", 0, EOF_EVENT_FLAG_RS_SECTION, 1);	//Add a temporary one
	}
	if(!eof_song_contains_event(sp, "noguitar", track, EOF_EVENT_FLAG_RS_SECTION, 1) && !eof_song_contains_event(sp, "noguitar", 0, EOF_EVENT_FLAG_RS_SECTION, 1))
	{	//If the user did not define a noguitar RS section that applies to either the track being exported or all tracks
		if(sp->beat[endbeat]->contained_rs_section_event >= 0)
		{	//If there is already a RS section defined on the first beat after the last note
			allegro_message("Warning:  There is no noguitar RS section, but the beat marker after the last note already has a section.\nYou should move that section because only one section per beat is exported.");
		}
		eof_log("\t! Adding missing noguitar RS section", 1);
		(void) eof_song_add_text_event(sp, endbeat, "noguitar", 0, EOF_EVENT_FLAG_RS_SECTION, 1);	//Add a temporary one
	}

	//Write the phrases
	eof_sort_events(sp);	//Re-sort events
	eof_process_beat_statistics(sp, track);	//Cache section name information into the beat structures (from the perspective of the specified track)
	for(ctr = 0, numsections = 0; ctr < sp->beats; ctr++)
	{	//For each beat in the chart
		if(sp->beat[ctr]->contained_section_event >= 0)
		{	//If this beat has a section event (RS phrase)
			numsections++;	//Update section marker instance counter
		}
	}
	sectionlistsize = eof_build_section_list(sp, &sectionlist, track);	//Build a list of all unique section markers (Rocksmith phrases) in the chart (from the perspective of the track being exported)
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <phrases count=\"%lu\">\n", sectionlistsize);	//Write the number of unique phrases
	(void) pack_fputs(buffer, fp);
	for(ctr = 0; ctr < sectionlistsize; ctr++)
	{	//For each of the entries in the unique section (RS phrase) list
		char * currentphrase = NULL;
		unsigned long startpos = 0, endpos = 0;		//Track the start and end position of the each instance of the phrase
		unsigned char maxdiff, ongoingmaxdiff = 0;	//Track the highest fully leveled difficulty used among all phraseinstances
		char started = 0;

		//Determine the highest maxdifficulty present among all instances of this phrase
		for(ctr2 = 0; ctr2 < sp->beats; ctr2++)
		{	//For each beat
			if((sp->beat[ctr2]->contained_section_event >= 0) || ((ctr2 + 1 >= eof_song->beats) && started))
			{	//If this beat contains a section event (Rocksmith phrase) or a phrase is in progress and this is the last beat, it marks the end of any current phrase and the potential start of another
				started = 0;
				endpos = sp->beat[ctr2]->pos - 1;	//Track this as the end position of the previous phrase marker
				if(currentphrase)
				{	//If the first instance of the phrase was already encountered
					if(!ustricmp(currentphrase, sp->text_event[sectionlist[ctr]]->text))
					{	//If the phrase that just ended is an instance of the phrase being written
						maxdiff = eof_find_fully_leveled_rs_difficulty_in_time_range(sp, track, startpos, endpos, 1);	//Find the maxdifficulty value for this phrase instance, converted to relative numbering
						if(maxdiff > ongoingmaxdiff)
						{	//If that phrase instance had a higher maxdifficulty than the other instances checked so far
							ongoingmaxdiff = maxdiff;	//Track it
						}
					}
				}
				started = 1;
				startpos = sp->beat[ctr2]->pos;	//Track the starting position
				currentphrase = sp->text_event[eof_song->beat[ctr2]->contained_section_event]->text;	//Track which phrase is being examined
			}
		}

		//Write the phrase definition using the highest difficulty found among all instances of the phrase
		expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->text_event[sectionlist[ctr]]->text, 32);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
		(void) snprintf(buffer, sizeof(buffer) - 1, "    <phrase disparity=\"0\" ignore=\"0\" maxDifficulty=\"%u\" name=\"%s\" solo=\"0\"/>\n", ongoingmaxdiff, buffer2);
		(void) pack_fputs(buffer, fp);
	}//For each of the entries in the unique section (RS phrase) list
	(void) pack_fputs("  </phrases>\n", fp);
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <phraseIterations count=\"%lu\">\n", numsections);	//Write the number of phrase instances
	(void) pack_fputs(buffer, fp);
	for(ctr = 0; ctr < sp->beats; ctr++)
	{	//For each beat in the chart
		if(sp->beat[ctr]->contained_section_event >= 0)
		{	//If this beat has a section event
			for(ctr2 = 0; ctr2 < sectionlistsize; ctr2++)
			{	//For each of the entries in the unique section list
				if(!ustricmp(sp->text_event[sp->beat[ctr]->contained_section_event]->text, sp->text_event[sectionlist[ctr2]]->text))
				{	//If this event matches a section marker entry
					phraseid = ctr2;
					break;
				}
			}
			if(ctr2 >= sectionlistsize)
			{	//If the section couldn't be found
				allegro_message("Error:  Couldn't find section in unique section list.  Aborting Rocksmith export.");
				eof_log("Error:  Couldn't find section in unique section list.  Aborting Rocksmith export.", 1);
				free(sectionlist);
				return 0;	//Return error
			}
			(void) snprintf(buffer, sizeof(buffer) - 1, "    <phraseIteration time=\"%.3f\" phraseId=\"%lu\" variation=\"\"/>\n", sp->beat[ctr]->fpos / 1000.0, phraseid);
			(void) pack_fputs(buffer, fp);
		}
	}
	(void) pack_fputs("  </phraseIterations>\n", fp);
	if(sectionlistsize)
	{	//If there were any entries in the unique section list
		free(sectionlist);	//Free the list now
	}

	//Write some unknown information
	(void) pack_fputs("  <newLinkedDiffs count=\"0\"/>\n", fp);
	(void) pack_fputs("  <linkedDiffs count=\"0\"/>\n", fp);
	(void) pack_fputs("  <phraseProperties count=\"0\"/>\n", fp);

	//Write chord templates
	chordlistsize = eof_build_chord_list(sp, track, &chordlist, 2);	//Build a list of all unique chords in the track
	if(!chordlistsize)
	{	//If there were no chords, write an empty chord template tag
		(void) pack_fputs("  <chordTemplates count=\"0\"/>\n", fp);
	}
	else
	{	//There were chords
		long fret0, fret1, fret2, fret3, fret4, fret5;	//Will store the fret number played on each string (-1 means the string is not played)
		long *fret[6] = {&fret0, &fret1, &fret2, &fret3, &fret4, &fret5};	//Allow the fret numbers to be accessed via array
		char *fingerunused = "-1";
		char *finger0, *finger1, *finger2, *finger3, *finger4, *finger5;	//Each will be set to either fingerunknown or fingerunused
		char **finger[6] = {&finger0, &finger1, &finger2, &finger3, &finger4, &finger5};	//Allow the finger strings to be accessed via array
		char finger0def[2] = "0", finger1def[2] = "1", finger2def[2] = "2", finger3def[2] = "3", finger4def[2] = "4", finger5def[2] = "5";	//Static strings for building manually-defined finger information
		char *fingerdef[6] = {finger0def, finger1def, finger2def, finger3def, finger4def, finger5def};	//Allow the fingerdef strings to be accessed via array
		unsigned long bitmask, shapenum;
		EOF_PRO_GUITAR_NOTE temp;	//Will have a matching chord shape definition's fingering applied to
		unsigned char *effective_fingering;	//Will point to either a note's own finger array or one of that of the temp pro guitar note structure above
		char arp[] = "-arp", no_arp[] = "";	//The suffix applied to the chord template's display name, depending on whether the template is for an arpeggio
		char *suffix;	//Will point to either arp[] or no_arp[]

		(void) snprintf(buffer, sizeof(buffer) - 1, "  <chordTemplates count=\"%lu\">\n", chordlistsize);
		(void) pack_fputs(buffer, fp);
		for(ctr = 0; ctr < chordlistsize; ctr++)
		{	//For each of the entries in the unique chord list
			notename[0] = '\0';	//Empty the note name string
			(void) eof_build_note_name(sp, track, chordlist[ctr], notename);	//Build the note name (if it exists) into notename[]

			effective_fingering = tp->note[chordlist[ctr]]->finger;	//By default, use the chord list entry's finger array
			memcpy(temp.frets, tp->note[chordlist[ctr]]->frets, 6);	//Clone the fretting of the chord into the temporary note
			temp.note = tp->note[chordlist[ctr]]->note;				//Clone the note mask
			if(eof_pro_guitar_note_fingering_valid(tp, chordlist[ctr]) != 1)
			{	//If the fingering for the note is not fully defined
				if(eof_lookup_chord_shape(tp->note[chordlist[ctr]], &shapenum, 0))
				{	//If a fingering for the chord can be found in the chord shape definitions
					eof_apply_chord_shape_definition(&temp, shapenum);	//Apply the matching chord shape definition's fingering
					effective_fingering = temp.finger;	//Use the matching chord shape definition's finger definitions
				}
			}

			for(ctr2 = 0, bitmask = 1; ctr2 < 6; ctr2++, bitmask <<= 1)
			{	//For each of the 6 supported strings
				if((eof_get_note_note(sp, track, chordlist[ctr]) & bitmask) && (ctr2 < tp->numstrings))
				{	//If the chord list entry uses this string (verifying that the string number is supported by the track)
					if((tp->note[chordlist[ctr]]->frets[ctr2] & 0x80) == 0)
					{	//If the string is not fret hand muted (ghost notes must be allowed so that arpeggio shapes can export)
						*(fret[ctr2]) = tp->note[chordlist[ctr]]->frets[ctr2] & 0x7F;	//Retrieve the fret played on this string (masking out the muting bit)
						if(*(fret[ctr2]))
						{	//If this string isn't played open
							*(fret[ctr2]) += tp->capo;	//Apply the capo position
						}
						if(effective_fingering[ctr2])
						{	//If the fingering for this string is defined
							char *temp = fingerdef[ctr2];	//Simplify logic below
							temp[0] = '0' + effective_fingering[ctr2];	//Convert decimal to ASCII
							temp[1] = '\0';	//Truncate string
							*(finger[ctr2]) = temp;
						}
						else
						{	//The fingering is not defined, regardless of whether the string is open or fretted
							*(finger[ctr2]) = fingerunused;		//Write a -1, this will allow the XML to compile even if the chord's fingering is incomplete/undefined
						}
					}
					else
					{	//The string is muted
						if(tp->note[chordlist[ctr]]->frets[ctr2] == 0xFF)
						{	//If no fret value is defined, assume 0
							*(fret[ctr2]) = 0;
						}
						else
						{	//The fret value is defined
							*(fret[ctr2]) = tp->note[chordlist[ctr]]->frets[ctr2] & 0x7F;	//Retrieve the fret played on this string (masking out the muting bit)
						}
						*(finger[ctr2]) = fingerunused;
					}
				}
				else
				{	//The chord list entry does not use this string
					*(fret[ctr2]) = -1;
					*(finger[ctr2]) = fingerunused;
				}
			}

			if(eof_is_partially_ghosted(sp, track, chordlist[ctr]))
			{	//If the chord list entry is partially ghosted (for arpeggio notation)
				suffix = arp;	//Apply the "-arp" suffix to the chord template's display name
			}
			else
			{
				suffix = no_arp;	//This chord template is not for an arpeggio chord, apply no suffix
			}
			expand_xml_text(buffer2, sizeof(buffer2) - 1, notename, 32 - 4);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field (reserve 4 characters for the "-arp" suffix)
			(void) snprintf(buffer, sizeof(buffer) - 1, "    <chordTemplate chordName=\"%s\" displayName=\"%s%s\" finger0=\"%s\" finger1=\"%s\" finger2=\"%s\" finger3=\"%s\" finger4=\"%s\" finger5=\"%s\" fret0=\"%ld\" fret1=\"%ld\" fret2=\"%ld\" fret3=\"%ld\" fret4=\"%ld\" fret5=\"%ld\"/>\n", buffer2, buffer2, suffix, finger0, finger1, finger2, finger3, finger4, finger5, fret0, fret1, fret2, fret3, fret4, fret5);
			(void) pack_fputs(buffer, fp);
		}//For each of the entries in the unique chord list
		(void) pack_fputs("  </chordTemplates>\n", fp);
	}//There were chords

	//Write some unknown information
	(void) pack_fputs("  <fretHandMuteTemplates count=\"0\"/>\n", fp);

	//DDC prefers when the XML pads partially complete measures by adding beats to finish the measure and then going one beat into the next measure
	originalbeatcount = sp->beats;	//Store the original beat count
	if(sp->beat[endbeat]->beat_within_measure)
	{	//If the first beat after the last note in this track isn't the first beat in a measure
		ctr = sp->beat[endbeat]->num_beats_in_measure - sp->beat[endbeat]->beat_within_measure;	//This is how many beats need to be after endbeat
		if(endbeat + ctr > sp->beats)
		{	//If the project doesn't have enough beats to accommodate this padding
			(void) eof_song_append_beats(sp, ctr);	//Append them to the end of the project
		}
	}
	eof_process_beat_statistics(sp, track);	//Rebuild beat stats

	//Write the beat timings
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <ebeats count=\"%lu\">\n", sp->beats);
	(void) pack_fputs(buffer, fp);
	for(ctr = 0; ctr < sp->beats; ctr++)
	{	//For each beat in the chart
		if(eof_get_ts(sp,&beatspermeasure,NULL,ctr) == 1)
		{	//If this beat has a time signature change
			beatcounter = 0;
		}
		if(!beatcounter)
		{	//If this is the first beat in a measure
			measurenum++;
			displayedmeasure = measurenum;
		}
		else
		{	//Otherwise the measure is displayed as -1 to indicate no change from the previous beat's measure number
			displayedmeasure = -1;
		}
		(void) snprintf(buffer, sizeof(buffer) - 1, "    <ebeat time=\"%.3f\" measure=\"%ld\"/>\n", sp->beat[ctr]->fpos / 1000.0, displayedmeasure);
		(void) pack_fputs(buffer, fp);
		beatcounter++;
		if(beatcounter >= beatspermeasure)
		{
			beatcounter = 0;
		}
	}
	(void) pack_fputs("  </ebeats>\n", fp);

	//Restore the original number of beats in the project in case any were added for DDC
	(void) eof_song_resize_beats(sp, originalbeatcount);

	//Write message boxes for the loading text song property (if defined) and each user defined popup message
	if(sp->tags->loading_text[0] != '\0')
	{	//If the loading text is defined
		char expanded_text[512];	//A string to expand the user defined text into
		(void) strftime(expanded_text, sizeof(expanded_text), sp->tags->loading_text, caltime);	//Expand any user defined calendar date/time tokens
		expand_xml_text(buffer2, sizeof(buffer2) - 1, expanded_text, 512);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field

		(void) eof_track_add_section(eof_song, track, EOF_RS_POPUP_MESSAGE, 0, 5100, 10100, 1, buffer2);	//Insert the expanded text as a popup message, setting the flag to nonzero to mark is as temporary
		eof_track_pro_guitar_sort_popup_messages(tp);	//Sort the popup messages
	}
	if(tp->popupmessages)
	{	//If at least one popup message is to be written
		unsigned long count, controlctr = 0;
		size_t stringlen;
		EOF_RS_CONTROL *controls = NULL;

		//Allocate memory for a list of control events
		count = tp->popupmessages * 2;	//Each popup message needs one control message to display and one to clear
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <controls count=\"%lu\">\n", count);
		(void) pack_fputs(buffer, fp);
		controls = malloc(sizeof(EOF_RS_CONTROL) * count);	//Allocate memory for a list of Rocksmith control events
		if(!controls)
		{
			eof_log("\tError saving:  Cannot allocate memory for control list", 1);
			return 0;	//Return failure
		}

		//Build the list of control events
		for(ctr = 0; ctr < tp->popupmessages; ctr++)
		{	//For each popup message
			//Add the popup message display control to the list
			expand_xml_text(buffer2, sizeof(buffer2) - 1, tp->popupmessage[ctr].name, EOF_SECTION_NAME_LENGTH);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
			stringlen = (size_t)snprintf(NULL, 0, "    <control time=\"%.3f\" code=\"ShowMessageBox(hint%lu, %s)\"/>\n", tp->popupmessage[ctr].start_pos / 1000.0, ctr + 1, buffer2) + 1;	//Find the number of characters needed to store this string
			controls[controlctr].str = malloc(stringlen + 1);	//Allocate memory to build the string
			if(!controls[controlctr].str)
			{
				eof_log("\tError saving:  Cannot allocate memory for control event", 1);
				while(controlctr > 0)
				{	//Free previously allocated strings
					free(controls[controlctr - 1].str);
					controlctr--;
				}
				free(controls);
				return 0;	//Return failure
			}
			(void) snprintf(controls[controlctr].str, stringlen, "    <control time=\"%.3f\" code=\"ShowMessageBox(hint%lu, %s)\"/>\n", tp->popupmessage[ctr].start_pos / 1000.0, ctr + 1, buffer2);
			controls[controlctr].pos = tp->popupmessage[ctr].start_pos;
			controlctr++;

			//Add the clear message control to the list
			stringlen = (size_t)snprintf(NULL, 0, "    <control time=\"%.3f\" code=\"ClearAllMessageBoxes()\"/>\n", tp->popupmessage[ctr].end_pos / 1000.0) + 1;	//Find the number of characters needed to store this string
			controls[controlctr].str = malloc(stringlen + 1);	//Allocate memory to build the string
			if(!controls[controlctr].str)
			{
				eof_log("\tError saving:  Cannot allocate memory for control event", 1);
				while(controlctr > 0)
				{	//Free previously allocated strings
					free(controls[controlctr - 1].str);
					controlctr--;
				}
				free(controls);
				return 0;	//Return failure
			}
			(void) snprintf(controls[controlctr].str, stringlen, "    <control time=\"%.3f\" code=\"ClearAllMessageBoxes()\"/>\n", tp->popupmessage[ctr].end_pos / 1000.0);
			controls[controlctr].pos = tp->popupmessage[ctr].end_pos;
			controlctr++;
		}

		//Sort, write and free the list of control events
		qsort(controls, (size_t)count, sizeof(EOF_RS_CONTROL), eof_song_qsort_control_events);
		for(ctr = 0; ctr < count; ctr++)
		{	//For each control event
			(void) pack_fputs(controls[ctr].str, fp);	//Write the control event string
			free(controls[ctr].str);	//Free the string
		}
		free(controls);	//Free the array

		(void) pack_fputs("  </controls>\n", fp);

		//Remove any loading text popup that was inserted into the track
		for(ctr = 0; ctr < tp->popupmessages; ctr++)
		{	//For each popup message
			if(tp->popupmessage[ctr].flags)
			{	//If the flags field was made nonzero
				eof_track_pro_guitar_delete_popup_message(tp, ctr);	//Delete this temporary popup message
				break;
			}
		}
	}//If at least one popup message is to be written

	//Write tone changes
	//Build and count the size of the list of unique tone names used, and empty the default tone string if it is not valid
	eof_track_rebuild_rs_tone_names_list_strings(track, 0);	//Build the tone names list without the (D) default tone name suffix
	if((tp->defaulttone[0] != '\0') && (eof_track_rs_tone_names_list_strings_num > 1))
	{	//If the default tone is valid and at least two different tone names are referenced among the tone changes
		unsigned long tonecount = 0;
		char *temp;
		char *effective_tone;	//The last tone exported

		//Make sure the default tone is placed at the beginning of the eof_track_rs_tone_names_list_strings[] list
		for(ctr = 1; ctr < eof_track_rs_tone_names_list_strings_num; ctr++)
		{	//For each unique tone name after the first
			if(!strcmp(eof_track_rs_tone_names_list_strings[ctr], tp->defaulttone))
			{	//If this is the track's default tone
				temp = eof_track_rs_tone_names_list_strings[0];	//Store the pointer to the first tone name in the list
				eof_track_rs_tone_names_list_strings[0] = eof_track_rs_tone_names_list_strings[ctr];	//Replace it with this one, which is the default tone
				eof_track_rs_tone_names_list_strings[ctr] = temp;	//And swap the previously first tone name in its place
				break;
			}
		}

		//Write the declarations for the default tone and the first two tones
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <tonebase>%s</tonebase>\n", eof_track_rs_tone_names_list_strings[0]);
		(void) pack_fputs(buffer, fp);
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <tonea>%s</tonea>\n", eof_track_rs_tone_names_list_strings[0]);
		(void) pack_fputs(buffer, fp);
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <toneb>%s</toneb>\n", eof_track_rs_tone_names_list_strings[1]);
		(void) pack_fputs(buffer, fp);

		//Write the third tone declaration if applicable
		if(eof_track_rs_tone_names_list_strings_num > 2)
		{	//If there is a third tone name
			(void) snprintf(buffer, sizeof(buffer) - 1, "  <tonec>%s</tonec>\n", eof_track_rs_tone_names_list_strings[2]);
			(void) pack_fputs(buffer, fp);
		}

		//Write the fourth tone declaration if applicable
		if(eof_track_rs_tone_names_list_strings_num > 3)
		{	//If there is a fourth tone name
			(void) snprintf(buffer, sizeof(buffer) - 1, "  <toned>%s</toned>\n", eof_track_rs_tone_names_list_strings[3]);
			(void) pack_fputs(buffer, fp);
		}

		//Count how many tone changes are valid to export
		effective_tone = tp->defaulttone;	//The default tone is automatically in effect at the start of the track
		for(ctr = 0; ctr < tp->tonechanges; ctr++)
		{	//For each tone change in the track
			for(ctr2 = 0; (ctr2 < eof_track_rs_tone_names_list_strings_num) && (ctr2 < 4); ctr2++)
			{	//For the first four unique tone names
				if(!strcmp(tp->tonechange[ctr].name, eof_track_rs_tone_names_list_strings[ctr2]))
				{	//If the tone change applies one of the four valid tone names
					if(strcmp(tp->tonechange[ctr].name, effective_tone))
					{	//If the tone being changed to isn't already in effect
						tonecount++;
						effective_tone = tp->tonechange[ctr].name;	//Track the tone that is in effect
					}
					break;	//Break from inner loop
				}
			}
		}

		//Write the tone changes that are valid to export
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <tones count=\"%lu\">\n", tonecount);
		(void) pack_fputs(buffer, fp);
		effective_tone = tp->defaulttone;	//The default tone is automatically in effect at the start of the track
		for(ctr = 0; ctr < tp->tonechanges; ctr++)
		{	//For each tone change in the track
			for(ctr2 = 0; (ctr2 < eof_track_rs_tone_names_list_strings_num) && (ctr2 < 4); ctr2++)
			{	//For the first four unique tone names
				if(!strcmp(tp->tonechange[ctr].name, eof_track_rs_tone_names_list_strings[ctr2]))
				{	//If the tone change applies one of the four valid tone names
					if(strcmp(tp->tonechange[ctr].name, effective_tone))
					{	//If the tone being changed to isn't already in effect
						(void) snprintf(buffer, sizeof(buffer) - 1, "    <tone time=\"%.3f\" name=\"%s\"/>\n", tp->tonechange[ctr].start_pos / 1000.0, tp->tonechange[ctr].name);
						(void) pack_fputs(buffer, fp);
						effective_tone = tp->tonechange[ctr].name;	//Track the tone that is in effect
					}
					break;	//Break from inner loop
				}
			}
		}
		(void) pack_fputs("  </tones>\n", fp);
	}//If the default tone is valid and at least two different tone names are referenced among the tone changes
	eof_track_destroy_rs_tone_names_list_strings();

	//Write sections
	for(ctr = 0, numsections = 0; ctr < sp->beats; ctr++)
	{	//For each beat in the chart
		if(sp->beat[ctr]->contained_rs_section_event >= 0)
		{	//If this beat has a Rocksmith section
			numsections++;	//Update Rocksmith section instance counter
		}
	}
	if(numsections)
	{	//If there is at least one Rocksmith section defined in the chart (which should be the case since default ones were inserted earlier if there weren't any)
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <sections count=\"%lu\">\n", numsections);
		(void) pack_fputs(buffer, fp);
		for(ctr = 0; ctr < sp->beats; ctr++)
		{	//For each beat in the chart
			if(sp->beat[ctr]->contained_rs_section_event >= 0)
			{	//If this beat has a Rocksmith section
				expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->text_event[sp->beat[ctr]->contained_rs_section_event]->text, 32);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
				(void) snprintf(buffer, sizeof(buffer) - 1, "    <section name=\"%s\" number=\"%d\" startTime=\"%.3f\"/>\n", buffer2, sp->beat[ctr]->contained_rs_section_event_instance_number, sp->beat[ctr]->fpos / 1000.0);
				(void) pack_fputs(buffer, fp);
			}
		}
		(void) pack_fputs("  </sections>\n", fp);
	}
	else
	{
		allegro_message("Error:  Default RS sections that were added are missing.  Skipping writing the <sections> tag.");
	}

	//Write events
	for(ctr = 0, numevents = 0; ctr < sp->text_events; ctr++)
	{	//For each event in the chart
		if(sp->text_event[ctr]->flags & EOF_EVENT_FLAG_RS_EVENT)
		{	//If the event is marked as a Rocksmith event
			if(!sp->text_event[ctr]->track || (sp->text_event[ctr]->track  == track))
			{	//If the event applies to the specified track
				numevents++;
			}
		}
	}
	if(numevents)
	{	//If there is at least one Rocksmith event defined in the chart
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <events count=\"%lu\">\n", numevents);
		(void) pack_fputs(buffer, fp);
		for(ctr = 0, numevents = 0; ctr < sp->text_events; ctr++)
		{	//For each event in the chart
			if(sp->text_event[ctr]->flags & EOF_EVENT_FLAG_RS_EVENT)
			{	//If the event is marked as a Rocksmith event
				if(!sp->text_event[ctr]->track || (sp->text_event[ctr]->track  == track))
				{	//If the event applies to the specified track
					expand_xml_text(buffer2, sizeof(buffer2) - 1, sp->text_event[ctr]->text, 256);	//Expand XML special characters into escaped sequences if necessary, and check against the maximum supported length of this field
					(void) snprintf(buffer, sizeof(buffer) - 1, "    <event time=\"%.3f\" code=\"%s\"/>\n", sp->beat[sp->text_event[ctr]->beat]->fpos / 1000.0, buffer2);
					(void) pack_fputs(buffer, fp);
				}
			}
		}
		(void) pack_fputs("  </events>\n", fp);
	}
	else
	{	//Otherwise write an empty events tag
		(void) pack_fputs("  <events count=\"0\"/>\n", fp);
	}

	//Write some unknown information
	(void) pack_fputs("  <transcriptionTrack difficulty=\"-1\">\n", fp);
	(void) pack_fputs("      <notes count=\"0\"/>\n", fp);
	(void) pack_fputs("      <chords count=\"0\"/>\n", fp);
	(void) pack_fputs("      <anchors count=\"0\"/>\n", fp);
	(void) pack_fputs("      <handShapes count=\"0\"/>\n", fp);
	(void) pack_fputs("  </transcriptionTrack>\n", fp);

	//Write note difficulties
	(void) snprintf(buffer, sizeof(buffer) - 1, "  <levels count=\"%u\">\n", numdifficulties);
	(void) pack_fputs(buffer, fp);
	for(ctr = 0, ctr2 = 0; ctr < 256; ctr++)
	{	//For each of the possible difficulties
		if(eof_track_diff_populated_status[ctr])
		{	//If this difficulty is populated
			unsigned long anchorcount;
			char anchorsgenerated = 0;	//Tracks whether anchors were automatically generated and will need to be deleted after export

			//Count the number of single notes and chords in this difficulty
			for(ctr3 = 0, numsinglenotes = 0, numchords = 0; ctr3 < tp->notes; ctr3++)
			{	//For each note in the track
				if(eof_get_note_type(sp, track, ctr3) == ctr)
				{	//If the note is in this difficulty
					unsigned long lanecount = eof_note_count_rs_lanes(sp, track, ctr3, 2);	//Count the number of non ghosted gems for this note
					if(lanecount == 1)
					{	//If the note has only one gem
						numsinglenotes++;	//Increment counter
					}
					else if(lanecount > 1)
					{	//If the note has multiple gems
						numchords++;	//Increment counter
					}
				}
			}

			//Write single notes
			(void) snprintf(buffer, sizeof(buffer) - 1, "    <level difficulty=\"%lu\">\n", ctr2);
			(void) pack_fputs(buffer, fp);
			ctr2++;	//Increment the populated difficulty level number
			if(numsinglenotes)
			{	//If there's at least one single note in this difficulty
				(void) snprintf(buffer, sizeof(buffer) - 1, "      <notes count=\"%lu\">\n", numsinglenotes);
				(void) pack_fputs(buffer, fp);
				for(ctr3 = 0; ctr3 < tp->notes; ctr3++)
				{	//For each note in the track
					if((eof_get_note_type(sp, track, ctr3) == ctr) && (eof_note_count_rs_lanes(sp, track, ctr3, 2) == 1))
					{	//If this note is in this difficulty and will export as a single note (only one gem has non ghosted status)
						for(stringnum = 0, bitmask = 1; stringnum < tp->numstrings; stringnum++, bitmask <<= 1)
						{	//For each string used in this track
							if((eof_get_note_note(sp, track, ctr3) & bitmask) && !(tp->note[ctr3]->ghost & bitmask))
							{	//If this string is used in this note and it is not ghosted
								unsigned long notepos;
								unsigned long fret;				//The fret number used for this string
								char tagend[2] = "/";			//If a note tag is to have a bendValues subtag, this string is emptied so that the note tag doesn't end in the same line

								(void) eof_get_rs_techniques(sp, track, ctr3, stringnum, &tech, 2);	//Determine techniques used by this note
								notepos = eof_get_note_pos(sp, track, ctr3);
								if(tp->note[ctr3]->frets[stringnum] == 0xFF)
								{	//If this is a string mute with no defined fret number
									fret = 0;	//Assume muted open note
								}
								else
								{	//Otherwise use the defined fret number
									fret = tp->note[ctr3]->frets[stringnum] & 0x7F;	//Get the fret value for this string (mask out the muting bit)
								}
								if(fret)
								{	//If this string isn't played open
									fret += tp->capo;	//Apply the capo position
								}
								if(!eof_pro_guitar_note_lowest_fret(tp, ctr3))
								{	//If this note contains no fretted strings
									if(tech.bend || (tech.slideto >= 0) || (tech.unpitchedslideto >= 0))
									{	//If it is also marked as a bend or slide note, omit these statuses because they're invalid for open notes
										tech.bend = tech.bendstrength_h = tech.bendstrength_q = 0;
										tech.slideto = -1;
										tech.unpitchedslideto = -1;
										if((*user_warned & 4) == 0)
										{	//If the user wasn't alerted that one or more open notes have these statuses improperly applied
											allegro_message("Warning:  At least one open note is marked with bend or slide status.\nThis is not supported, so these statuses are being omitted for such notes.");
											*user_warned |= 4;
										}
									}
								}
								if(tech.bend)
								{	//If the note is a bend, the note tag must not end on the same line as it will have a bendValues subtag
									tagend[0] = '\0';	//Drop the / from the string
								}
								(void) snprintf(buffer, sizeof(buffer) - 1, "        <note time=\"%.3f\" linkNext=\"%d\" accent=\"%d\" bend=\"%d\" fret=\"%lu\" hammerOn=\"%d\" harmonic=\"%d\" hopo=\"%d\" ignore=\"0\" leftHand=\"-1\" mute=\"%d\" palmMute=\"%d\" pluck=\"%d\" pullOff=\"%d\" slap=\"%d\" slideTo=\"%ld\" string=\"%lu\" sustain=\"%.3f\" tremolo=\"%d\" harmonicPinch=\"%d\" pickDirection=\"0\" rightHand=\"-1\" slideUnpitchTo=\"%ld\" tap=\"%d\" vibrato=\"%d\"%s>\n", (double)notepos / 1000.0, tech.linknext, tech.accent, tech.bend, fret, tech.hammeron, tech.harmonic, tech.hopo, tech.stringmute, tech.palmmute, tech.pop, tech.pulloff, tech.slap, tech.slideto, stringnum, (double)tech.length / 1000.0, tech.tremolo, tech.pinchharmonic, tech.unpitchedslideto, tech.tap, tech.vibrato, tagend);
								(void) pack_fputs(buffer, fp);
								if(tech.bend)
								{	//If the note is a bend, write the bendValues subtag and close the note tag
									(void) pack_fputs("          <bendValues count=\"1\">\n", fp);
									(void) snprintf(buffer, sizeof(buffer) - 1, "            <bendValue time=\"%.3f\" step=\"%.3f\"/>\n", (((double)notepos + ((double)tech.length / 3.0)) / 1000.0), (double)tech.bendstrength_q / 2.0);	//Write a bend point 1/3 into the note
									(void) pack_fputs(buffer, fp);
									(void) pack_fputs("          </bendValues>\n", fp);
									(void) pack_fputs("        </note>\n", fp);
								}
								break;	//Only one note entry is valid for each single note, so break from loop
							}//If this string is used in this note and it is not ghosted
						}//For each string used in this track
					}//If this note is in this difficulty and is a single note (and not a chord)
				}//For each note in the track
				(void) pack_fputs("      </notes>\n", fp);
			}//If there's at least one single note in this difficulty
			else
			{	//There are no single notes in this difficulty, write an empty notes tag
				(void) pack_fputs("      <notes count=\"0\"/>\n", fp);
			}

			//Write chords
			if(numchords)
			{	//If there's at least one chord in this difficulty
				unsigned long chordid, flags;
				unsigned long lastchordid = 0;	//Stores the previous written chord's ID, so that when the ID changes, chordNote subtags can be forced to be written
				char *upstrum = "up";
				char *downstrum = "down";
				char *direction;	//Will point to either upstrum or downstrum as appropriate
				double notepos;
				char highdensity;	//Various criteria determine whether the highDensity boolean property is set to true
				char chordnote;		//Tracks whether a chordNote subtag is to be written

				(void) snprintf(buffer, sizeof(buffer) - 1, "      <chords count=\"%lu\">\n", numchords);
				(void) pack_fputs(buffer, fp);
				for(ctr3 = 0; ctr3 < tp->notes; ctr3++)
				{	//For each note in the track
					if((eof_get_note_type(sp, track, ctr3) == ctr) && (eof_note_count_rs_lanes(sp, track, ctr3, 2) > 1))
					{	//If this note is in this difficulty and will export as a chord (at least two non ghosted gems)
						char tagend[2] = "/";	//If a chord tag is to have a chordNote subtag, this string is emptied so that the chord tag doesn't end in the same line
						chordnote = 0;	//Reset this status

						for(ctr4 = 0; ctr4 < chordlistsize; ctr4++)
						{	//For each of the entries in the unique chord list
							if(!eof_note_compare_simple(sp, track, ctr3, chordlist[ctr4]))
							{	//If this note matches a chord list entry
								chordid = ctr4;	//Store the chord list entry number
								break;
							}
						}
						if(ctr4 >= chordlistsize)
						{	//If the chord couldn't be found
							allegro_message("Error:  Couldn't match chord with chord template while exporting chords.  Aborting Rocksmith 2 export.");
							eof_log("Error:  Couldn't match chord with chord template while exporting chords.  Aborting Rocksmith 2 export.", 1);
							if(chordlist)
							{	//If the chord list was built
								free(chordlist);
							}
							return 0;	//Return error
						}
						flags = tp->note[ctr3]->flags;	//Simplify
						if(flags & EOF_PRO_GUITAR_NOTE_FLAG_UP_STRUM)
						{	//If this note explicitly strums up
							direction = upstrum;	//Set the direction string to match
						}
						else
						{	//Otherwise the direction defaults to down
							direction = downstrum;
						}
						(void) eof_get_rs_techniques(sp, track, ctr3, 0, &tech, 2);			//Determine techniques used by this chord
						highdensity = eof_note_has_high_chord_density(sp, track, ctr3, 2);	//Determine whether the chord will export with high density
						notepos = (double)tp->note[ctr3]->pos / 1000.0;
						if((chordid != lastchordid) || !highdensity)
						{	//If this chord's ID is different from that of the previous chord or it meets the normal criteria for a low density chord
							chordnote = 1;		//Ensure chordNote subtags are written
							highdensity = 0;	//Ensure the chord tag is written to reflect low density
							tagend[0] = '\0';	//Drop the / from the string
						}
						(void) snprintf(buffer, sizeof(buffer) - 1, "        <chord time=\"%.3f\" linkNext=\"%d\" accent=\"%d\" chordId=\"%lu\" fretHandMute=\"%d\" highDensity=\"%d\" ignore=\"0\" palmMute=\"%d\" hopo=\"%d\" strum=\"%s\"%s>\n", notepos, tech.linknext, tech.accent, chordid, tech.stringmute, highdensity, tech.palmmute, tech.hopo, direction, tagend);
						(void) pack_fputs(buffer, fp);
						if(chordnote)
						{	//If chordNote tags are to be written
							unsigned long stringnum, bitmask;
							unsigned char *finger = tp->note[chordlist[chordid]]->finger;	//Point to this chord's template's finger array
							long fingernum;
							long slidediff = 0;	//Used to find how many frets a slide is, so it can be evenly applied to all fretted strings in a chord
							long unpitchedslidediff = 0;	//Same as the above, but for unpitched slide tracking

							for(stringnum = 0, bitmask = 1; stringnum < tp->numstrings; stringnum++, bitmask <<= 1)
							{	//For each string used in this track
								if((eof_get_note_note(sp, track, ctr3) & bitmask) && !(tp->note[ctr3]->ghost & bitmask))
								{	//If this string is used in this note and it is not ghosted
									char tagend[2] = "/";	//If a chordNote tag is to have a bendValues subtag, this string is emptied so that the note tag doesn't end in the same line
									long fret;				//The fret number used for this string (uses signed math, keep it a signed int type)

									(void) eof_get_rs_techniques(sp, track, ctr3, stringnum, &tech, 2);	//Determine techniques used by this note
									if(tp->note[ctr3]->frets[stringnum] == 0xFF)
									{	//If this is a string mute with no defined fret number
										fret = 0;	//Assume muted open note
									}
									else
									{	//Otherwise use the defined fret number
										fret = tp->note[ctr3]->frets[stringnum] & 0x7F;	//Get the fret value for this string (mask out the muting bit)
									}
									if(fret)
									{	//If this string isn't played open
										fret += tp->capo;	//Apply the capo position
									}
									if(tp->note[ctr3]->frets[stringnum] & 0x80)
									{	//If the note is string muted
										tech.stringmute = 1;	//Ensure the chordNote indicates this string is muted
									}
									else
									{	//The note is not string muted
										tech.stringmute = 0;
									}
									if(tech.slideto >= 0)
									{	//If the chord has slide technique
										if(!slidediff && fret)
										{	//If this is the first fretted string in the chord
											slidediff = tech.slideto - fret;	//Determine how many frets the slide is (negative is a downward slide)
										}
										if(fret)
										{	//If this string is fretted
											tech.slideto = fret + slidediff;	//Get the correct ending fret for this string's slide
										}
										else
										{	//Otherwise this string does not slide
											tech.slideto = -1;
										}
									}
									if(tech.unpitchedslideto >= 0)
									{	//If the chord has unpitched slide technique
										if(!unpitchedslidediff && fret)
										{	//If this is the first fretted string in the chord
											unpitchedslidediff = tech.unpitchedslideto - fret;	//Determine how many frets the unpitched slide is
										}
										if(fret)
										{	//If this string is fretted
											tech.unpitchedslideto = fret + unpitchedslidediff;	//Get the correct ending fret for this string's unpitched slide
										}
										else
										{	//Otherwise this string does not slide
											tech.unpitchedslideto = -1;
										}
									}
									if(tech.bend || (tech.slideto >= 0) || (tech.unpitchedslideto >= 0))
									{	//If this note is marked as a bend or slide note
										if(!eof_pro_guitar_note_lowest_fret(tp, ctr3))
										{	//If this note also contains no fretted strings, omit these statuses because they're invalid for open notes
											tech.bend = tech.bendstrength_h = tech.bendstrength_q = 0;
											tech.slideto = -1;
											tech.unpitchedslideto = -1;
											tech.length = 0;	//chordNotes should have no sustain unless they use bend or slide technique
											if((*user_warned & 4) == 0)
											{	//If the user wasn't alerted that one or more open notes have these statuses improperly applied
												allegro_message("Warning:  At least one open note is marked with bend or slide status.\nThis is not supported, so these statuses are being omitted for such notes.");
												*user_warned |= 4;
											}
										}
									}
									else if(!tech.tremolo)
									{	//Otherwise if the chord also does not have tremolo status (which is required for the technique to display in game)
										tech.length = 0;	//Force the chordNotes to have no sustain
									}
									if(tech.bend)
									{	//If the note is a bend, the note tag must not end on the same line as it will have a bendValues subtag
										tagend[0] = '\0';	//Drop the / from the string
									}
									if(finger[stringnum])
									{	//If this string is fretted
										fingernum = finger[stringnum];
									}
									else
									{	//This string is played open
										fingernum = -1;
									}
									(void) snprintf(buffer, sizeof(buffer) - 1, "          <chordNote time=\"%.3f\" linkNext=\"%d\" accent=\"%d\" bend=\"%d\" fret=\"%ld\" hammerOn=\"%d\" harmonic=\"%d\" hopo=\"%d\" ignore=\"0\" leftHand=\"%ld\" mute=\"%d\" palmMute=\"%d\" pluck=\"%d\" pullOff=\"%d\" slap=\"%d\" slideTo=\"%ld\" string=\"%lu\" sustain=\"%.3f\" tremolo=\"%d\" harmonicPinch=\"%d\" pickDirection=\"0\" rightHand=\"-1\" slideUnpitchTo=\"%ld\" tap=\"%d\" vibrato=\"%d\"%s>\n", notepos, tech.linknext, tech.accent, tech.bend, fret, tech.hammeron, tech.harmonic, tech.hopo, fingernum, tech.stringmute, tech.palmmute, tech.pop, tech.pulloff, tech.slap, tech.slideto, stringnum, (double)tech.length / 1000.0, tech.tremolo, tech.pinchharmonic, tech.unpitchedslideto, tech.tap, tech.vibrato, tagend);
									(void) pack_fputs(buffer, fp);
									if(tech.bend)
									{	//If the note is a bend, write the bendValues subtag and close the note tag
										(void) pack_fputs("            <bendValues count=\"1\">\n", fp);
										(void) snprintf(buffer, sizeof(buffer) - 1, "              <bendValue time=\"%.3f\" step=\"%.3f\"/>\n", (((double)notepos + ((double)tech.length / 3.0)) / 1000.0), (double)tech.bendstrength_q / 2.0);	//Write a bend point 1/3 into the note
										(void) pack_fputs(buffer, fp);
										(void) pack_fputs("            </bendValues>\n", fp);
										(void) pack_fputs("          </chordNote>\n", fp);
									}
								}//If this string is used in this note and it is not ghosted
							}//For each string used in this track
							(void) pack_fputs("        </chord>\n", fp);
						}//If chordNote tags are to be written
						lastchordid = chordid;
					}//If this note is in this difficulty and is a chord
				}//For each note in the track
				(void) pack_fputs("      </chords>\n", fp);
			}
			else
			{	//There are no chords in this difficulty, write an empty chords tag
				(void) pack_fputs("      <chords count=\"0\"/>\n", fp);
			}

			//Write other stuff
			(void) pack_fputs("      <fretHandMutes count=\"0\"/>\n", fp);

			//Write anchors (fret hand positions)
			for(ctr3 = 0, anchorcount = 0; ctr3 < tp->handpositions; ctr3++)
			{	//For each hand position defined in the track
				if(tp->handposition[ctr3].difficulty == ctr)
				{	//If the hand position is in this difficulty
					anchorcount++;
				}
			}
			if(!anchorcount)
			{	//If there are no anchors in this track difficulty, automatically generate them
				if((*user_warned & 1) == 0)
				{	//If the user wasn't alerted that one or more track difficulties have no fret hand positions defined
					allegro_message("Warning:  At least one track difficulty has no fret hand positions defined.  They will be created automatically.");
					*user_warned |= 1;
				}
				eof_fret_hand_position_list_dialog_undo_made = 1;	//Ensure no undo state is written during export
				eof_generate_efficient_hand_positions(sp, track, ctr, 0, 0);	//Generate the fret hand positions for the track difficulty being currently written (use a static fret range tolerance of 4 for all frets)
				anchorsgenerated = 1;
			}
			for(ctr3 = 0, anchorcount = 0; ctr3 < tp->handpositions; ctr3++)	//Re-count the hand positions
			{	//For each hand position defined in the track
				if(tp->handposition[ctr3].difficulty == ctr)
				{	//If the hand position is in this difficulty
					anchorcount++;
				}
			}
			if(anchorcount)
			{	//If there's at least one anchor in this difficulty
				(void) snprintf(buffer, sizeof(buffer) - 1, "      <anchors count=\"%lu\">\n", anchorcount);
				(void) pack_fputs(buffer, fp);
				for(ctr3 = 0; ctr3 < tp->handpositions; ctr3++)
				{	//For each hand position defined in the track
					if(tp->handposition[ctr3].difficulty == ctr)
					{	//If the hand position is in this difficulty
						unsigned long highest, nextanchorpos, width = 4, fret;

						nextanchorpos = tp->note[tp->notes - 1]->pos + 1;	//In case there are no other anchors, initialize this to reflect covering all remaining notes
						for(ctr4 = ctr3 + 1; ctr4 < tp->handpositions; ctr4++)
						{	//For the remainder of the fret hand positions
							if(tp->handposition[ctr4].difficulty == ctr)
							{	//If the hand position is in this difficulty
								nextanchorpos = tp->handposition[ctr4].start_pos;	//Track its position
								break;
							}
						}
						highest = eof_get_highest_fret_in_time_range(sp, track, ctr, tp->handposition[ctr3].start_pos, nextanchorpos - 1);	//Find the highest fret number used within the scope of this fret hand position
						if(highest > tp->handposition[ctr3].end_pos + 3)
						{	//If any notes within the scope of this fret hand position require the anchor width to be increased beyond 4 frets
							width = highest - tp->handposition[ctr3].end_pos + 1;	//Determine the minimum needed width
						}
						fret = tp->handposition[ctr3].end_pos + tp->capo;	////Apply the capo position
						(void) snprintf(buffer, sizeof(buffer) - 1, "        <anchor time=\"%.3f\" fret=\"%lu\" width=\"%lu.000\"/>\n", (double)tp->handposition[ctr3].start_pos / 1000.0, fret, width);
						(void) pack_fputs(buffer, fp);
					}
				}
				(void) pack_fputs("      </anchors>\n", fp);
			}
			else
			{	//There are no anchors in this difficulty, write an empty anchors tag
				(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "Error:  Failed to automatically generate fret hand positions for level %lu of\n\"%s\" during MIDI export.", ctr2, fn);
				eof_log(eof_log_string, 1);
				allegro_message(eof_log_string);
				(void) pack_fputs("      <anchors count=\"0\"/>\n", fp);
			}
			if(anchorsgenerated)
			{	//If anchors were automatically generated for this track difficulty, remove them now
				for(ctr3 = tp->handpositions; ctr3 > 0; ctr3--)
				{	//For each hand position defined in the track, in reverse order
					if(tp->handposition[ctr3 - 1].difficulty == ctr)
					{	//If the hand position is in this difficulty
						eof_pro_guitar_track_delete_hand_position(tp, ctr3 - 1);	//Delete the hand position
					}
				}
			}

			//Write hand shapes
			//Count the number of hand shapes to write
			handshapectr = 0;
			for(ctr3 = 0; ctr3 < tp->notes; ctr3++)
			{	//For each note in the track
				if((eof_get_note_type(sp, track, ctr3) == ctr) && ((eof_note_count_rs_lanes(sp, track, ctr3, 1) > 1) || eof_is_partially_ghosted(sp, track, ctr3)))
				{	//If this note is in this difficulty and will export as a chord (at least two non ghosted/muted gems) or an arpeggio handshape
					unsigned long chord = ctr3;	//Store a copy of this note number because ctr3 will be manipulated below
					int ghost1, ghost2;

					ghost1 = eof_is_partially_ghosted(sp, track, ctr3);	//Determine whether this chord is partially ghosted

					//Find this chord's ID
					for(ctr4 = 0; ctr4 < chordlistsize; ctr4++)
					{	//For each of the entries in the unique chord list
						if(!eof_note_compare_simple(sp, track, ctr3, chordlist[ctr4]))
						{	//If this note matches a chord list entry
							ghost2 = eof_is_partially_ghosted(sp, track, chordlist[ctr4]);	//Determine whether the chord list entry is partially ghosted
							if(ghost1 == ghost2)
							{	//If the chord and the chord list entry are both either partially ghosted or not ghosted at all, consider it a match
								chordid = ctr4;	//Store the chord list entry number
								break;
							}
						}
					}
					if(ctr4 >= chordlistsize)
					{	//If the chord couldn't be found
						allegro_message("Error:  Couldn't match chord with chord template while counting handshapes.  Aborting Rocksmith 2 export.");
						eof_log("Error:  Couldn't match chord with chord template while counting handshapes.  Aborting Rocksmith 2 export.", 1);
						if(chordlist)
						{	//If the chord list was built
							free(chordlist);
						}
						return 0;	//Return error
					}
					handshapestart = eof_get_note_pos(sp, track, ctr3);	//Store this chord's start position

					//If this chord is at the beginning of an arpeggio phrase, skip the rest of the notes in that phrase
					for(ctr5 = 0; ctr5 < tp->arpeggios; ctr5++)
					{	//For each arpeggio phrase in the track
						if(((tp->note[ctr3]->pos + 10 >= tp->arpeggio[ctr5].start_pos) && (tp->note[ctr3]->pos <= tp->arpeggio[ctr5].start_pos + 10)) && (tp->note[ctr3]->type == tp->arpeggio[ctr5].difficulty))
						{	//If this chord's start position is within 10ms of an arpeggio phrase in this track difficulty
							while(1)
							{
								nextnote = eof_fixup_next_note(sp, track, ctr3);
								if((nextnote >= 0) && (tp->note[nextnote]->pos <= tp->arpeggio[ctr5].end_pos))
								{	//If there is another note and it is in the same arpeggio phrase
									ctr3 = nextnote;	//Iterate to that note, and check subsequent notes to see if they are also in the phrase
								}
								else
								{	//The next note (if any) is not in the arpeggio phrase
									break;	//Break from while loop
								}
							}
							break;	//Break from for loop
						}
					}

					//Examine subsequent notes to see if they match this chord
					while(1)
					{
						nextnote = eof_fixup_next_note(sp, track, ctr3);
						if((nextnote >= 0) && (eof_note_count_rs_lanes(sp, track, nextnote, 2) > 1))
						{	//If there is another note and it is a chord
							(void) eof_get_rs_techniques(sp, track, nextnote, 0, &tech, 2);	//Determine techniques used by the next note
							if(tech.slideto >= 0)
							{	//If the next note is a chord bend, it will require its own handshape to work in-game
								handshapeend = eof_get_note_pos(sp, track, ctr3) + eof_get_note_length(sp, track, ctr3);	//End the hand shape at the end of this chord
								break;	//Break from while loop
							}
						}
						if((nextnote >= 0) && (!eof_note_compare_simple(sp, track, chord, nextnote) || eof_is_string_muted(sp, track, nextnote)) && !eof_is_partially_ghosted(sp, track, nextnote))
						{	//If there is another note, it either matches this chord or is completely string muted and it is not partially ghosted (an arpeggio)
							ctr3 = nextnote;	//Iterate to that note, and check subsequent notes to see if they match
						}
						else
						{	//The next note (if any) is not a repeat of this note and is not completely string muted
							handshapeend = eof_get_note_pos(sp, track, ctr3) + eof_get_note_length(sp, track, ctr3);	//End the hand shape at the end of this chord
							break;	//Break from while loop
						}
					}

					handshapectr++;	//One more hand shape has been counted
				}//If this note is in this difficulty and will export as a chord (at least two non ghosted gems)
			}//For each note in the track

			if(handshapectr)
			{	//If there was at least one hand shape to write
				//Write the hand shapes
				(void) snprintf(buffer, sizeof(buffer) - 1, "      <handShapes count=\"%lu\">\n", handshapectr);
				(void) pack_fputs(buffer, fp);
				for(ctr3 = 0; ctr3 < tp->notes; ctr3++)
				{	//For each note in the track
					if((eof_get_note_type(sp, track, ctr3) == ctr) && ((eof_note_count_rs_lanes(sp, track, ctr3, 1) > 1) || eof_is_partially_ghosted(sp, track, ctr3)))
					{	//If this note is in this difficulty and will export as a chord (at least two non ghosted/muted gems) or an arpeggio handshape
						unsigned long chord = ctr3;	//Store a copy of this note number because ctr3 will be manipulated below
						int ghost1, ghost2;

						ghost1 = eof_is_partially_ghosted(sp, track, ctr3);	//Determine whether this chord is partially ghosted

						//Find this chord's ID
						for(ctr4 = 0; ctr4 < chordlistsize; ctr4++)
						{	//For each of the entries in the unique chord list
							if(!eof_note_compare_simple(sp, track, ctr3, chordlist[ctr4]))
							{	//If this note matches a chord list entry
								ghost2 = eof_is_partially_ghosted(sp, track, chordlist[ctr4]);	//Determine whether the chord list entry is partially ghosted
								if(ghost1 == ghost2)
								{	//If the chord and the chord list entry are both either partially ghosted or not ghosted at all, consider it a match
									chordid = ctr4;	//Store the chord list entry number
									break;
								}
							}
						}
						if(ctr4 >= chordlistsize)
						{	//If the chord couldn't be found
							allegro_message("Error:  Couldn't match chord with chord template while writing handshapes.  Aborting Rocksmith 2 export.");
							eof_log("Error:  Couldn't match chord with chord template while writing handshapes.  Aborting Rocksmith 2 export.", 1);
							if(chordlist)
							{	//If the chord list was built
								free(chordlist);
							}
							return 0;	//Return error
						}
						handshapestart = eof_get_note_pos(sp, track, ctr3);	//Store this chord's start position (in seconds)

						//If this chord is at the beginning of an arpeggio phrase, skip the rest of the notes in that phrase
						for(ctr5 = 0; ctr5 < tp->arpeggios; ctr5++)
						{	//For each arpeggio phrase in the track
							if(((tp->note[ctr3]->pos + 10 >= tp->arpeggio[ctr5].start_pos) && (tp->note[ctr3]->pos <= tp->arpeggio[ctr5].start_pos + 10)) && (tp->note[ctr3]->type == tp->arpeggio[ctr5].difficulty))
							{	//If this chord's start position is within 10ms of an arpeggio phrase in this track difficulty
								while(1)
								{
									nextnote = eof_fixup_next_note(sp, track, ctr3);
									if((nextnote >= 0) && (tp->note[nextnote]->pos <= tp->arpeggio[ctr5].end_pos))
									{	//If there is another note and it is in the same arpeggio phrase
										ctr3 = nextnote;	//Iterate to that note, and check subsequent notes to see if they are also in the phrase
									}
									else
									{	//The next note (if any) is not in the arpeggio phrase
										break;	//Break from while loop
									}
								}
								break;	//Break from for loop
							}
						}

						//Examine subsequent notes to see if they match this chord
						while(1)
						{
							nextnote = eof_fixup_next_note(sp, track, ctr3);
							if((nextnote >= 0) && (eof_note_count_rs_lanes(sp, track, nextnote, 2) > 1))
							{	//If there is another note and it is a chord
								(void) eof_get_rs_techniques(sp, track, nextnote, 0, &tech, 2);	//Determine techniques used by the next note
								if(tech.slideto >= 0)
								{	//If the next note is a chord bend, it will require its own handshape to work in-game
									handshapeend = eof_get_note_pos(sp, track, ctr3) + eof_get_note_length(sp, track, ctr3);	//End the hand shape at the end of this chord
									break;	//Break from while loop
								}
							}
							if((nextnote >= 0) && (!eof_note_compare_simple(sp, track, chord, nextnote) || eof_is_string_muted(sp, track, nextnote)) && !eof_is_partially_ghosted(sp, track, nextnote))
							{	//If there is another note, it either matches this chord or is completely string muted and it is not partially ghosted (an arpeggio)
								ctr3 = nextnote;	//Iterate to that note, and check subsequent notes to see if they match
							}
							else
							{	//The next note (if any) is not a repeat of this note and is not completely string muted
								handshapeend = eof_get_note_pos(sp, track, ctr3) + eof_get_note_length(sp, track, ctr3);	//End the hand shape at the end of this chord

								if((handshapeend - handshapestart < 56) && (handshapestart + 56 < eof_get_note_pos(sp, track, nextnote)))
								{	//If the hand shape would be shorter than 56ms, and the next note is further than 56ms away
									handshapeend = eof_get_note_pos(sp, track, ctr3) + 56;	//Pad the hand shape to 56ms
								}
								break;	//Break from while loop
							}
						}

						//Write this hand shape
						(void) snprintf(buffer, sizeof(buffer) - 1, "        <handShape chordId=\"%lu\" endTime=\"%.3f\" startTime=\"%.3f\"/>\n", chordid, (double)handshapeend / 1000.0, (double)handshapestart / 1000.0);
						(void) pack_fputs(buffer, fp);
					}//If this note is in this difficulty and will export as a chord (at least two non ghosted gems)
				}//For each note in the track
				(void) pack_fputs("      </handShapes>\n", fp);
			}
			else
			{	//There are no chords in this difficulty, write an empty hand shape tag
				(void) pack_fputs("      <handShapes count=\"0\"/>\n", fp);
			}

			//Write closing level tag
			(void) pack_fputs("    </level>\n", fp);
		}//If this difficulty is populated
	}//For each of the available difficulties
	(void) pack_fputs("  </levels>\n", fp);
	(void) pack_fputs("</song>\n", fp);
	(void) pack_fclose(fp);

	//Generate showlights XML file for this track
	if((sp->track[track]->flags & EOF_TRACK_FLAG_ALT_NAME) && (sp->track[track]->altname[0] != '\0'))
	{	//If the track has an alternate name
		arrangement_name = sp->track[track]->altname;
	}
	else
	{	//Otherwise use the track's native name
		arrangement_name = sp->track[track]->name;
	}
	(void) snprintf(buffer, 600, "%s_RS2_showlights.xml", arrangement_name);
	(void) replace_filename(fn, fn, buffer, 1024);
	eof_export_rocksmith_showlights(sp, fn, track);

	//Cleanup
	if(chordlist)
	{	//If the chord list was built
		free(chordlist);
	}
	//Remove all temporary text events that were added
	for(ctr = sp->text_events; ctr > 0; ctr--)
	{	//For each text event (in reverse order)
		if(sp->text_event[ctr - 1]->is_temporary)
		{	//If this text event has been marked as temporary
			eof_song_delete_text_event(sp, ctr - 1);	//Delete it
		}
	}
	eof_sort_events(sp);	//Re-sort events

	return 1;	//Return success
}

void eof_pro_guitar_track_fix_fingerings(EOF_PRO_GUITAR_TRACK *tp, char *undo_made)
{
	unsigned long ctr2, ctr3;
	unsigned char *array;	//Points to the finger array being replicated to matching notes
	int retval;

	if(!tp)
		return;	//Invalid parameter

	for(ctr2 = 0; ctr2 < tp->notes; ctr2++)
	{	//For each note in the track (outer loop)
		retval = eof_pro_guitar_note_fingering_valid(tp, ctr2);
		if(retval == 1)
		{	//If the note's fingering was complete
			if(eof_note_count_colors_bitmask(tp->note[ctr2]->note) > 1)
			{	//If this note is a chord
				array = tp->note[ctr2]->finger;
				for(ctr3 = 0; ctr3 < tp->notes; ctr3++)
				{	//For each note in the track (inner loop)
					if((ctr2 != ctr3) && (eof_pro_guitar_note_compare(tp, ctr2, tp, ctr3) == 0))
					{	//If this note matches the note being examined in the outer loop, and we're not comparing the note to itself
						if(eof_pro_guitar_note_fingering_valid(tp, ctr3) != 1)
						{	//If the fingering of the inner loop's note is invalid/undefined
							if(undo_made && !(*undo_made))
							{	//If an undo hasn't been made yet
								eof_prepare_undo(EOF_UNDO_TYPE_NONE);
								*undo_made = 1;
							}
							memcpy(tp->note[ctr3]->finger, array, 8);	//Overwrite it with the current finger array
						}
						else
						{	//The inner loop's note has a valid fingering array defined
							array = tp->note[ctr3]->finger;	//Use this finger array for remaining matching notes in the track
						}
					}
				}//For each note in the track (inner loop)
			}
		}
		else if(retval == 0)
		{	//If the note's fingering was defined, but invalid
			if(undo_made && !(*undo_made))
			{	//If an undo hasn't been made yet
				eof_prepare_undo(EOF_UNDO_TYPE_NONE);
				*undo_made = 1;
			}
			memset(tp->note[ctr2], 0, 8);	//Clear it
		}
	}
}

int eof_pro_guitar_note_fingering_valid(EOF_PRO_GUITAR_TRACK *tp, unsigned long note)
{
	unsigned long ctr, bitmask;
	char string_finger_defined = 0, string_finger_undefined = 0, all_strings_open = 1;

	if(!tp || (note >= tp->notes))
		return 0;	//Invalid parameters

	for(ctr = 0, bitmask = 1; ctr < tp->numstrings; ctr++, bitmask <<= 1)
	{	//For each string supported by this track
		if(tp->note[note]->note & bitmask)
		{	//If this string is used
			if((tp->note[note]->frets[ctr] & 0x80) == 0)
			{	//If the string isn't muted
				if(tp->note[note]->frets[ctr] != 0)
				{	//If the string isn't being played open, it requires a fingering
					all_strings_open = 0;	//Track that the note used at least one fretted string
					if(tp->note[note]->finger[ctr] != 0)
					{	//If this string has a finger definition
						string_finger_defined = 1;	//Track that a string was defined
					}
					else
					{	//This string does not have a finger definition
						string_finger_undefined = 1;	//Track that a string was undefined
					}
				}
				else
				{	//If the string is being played open, it must not have a finger defined
					if(tp->note[note]->finger[ctr] != 0)
					{	//If this string has a finger definition
						string_finger_defined = string_finger_undefined = 1;	//Set an error condition
						break;
					}
				}
			}
		}
	}

	if(all_strings_open && !string_finger_defined)
	{	//If the note only had open strings played, and no fingering was defined, this is valid
		return 1;	//Return fingering valid
	}
	if(string_finger_defined && string_finger_undefined)
	{	//If a note only had partial finger definition
		return 0;	//Return fingering invalid
	}
	if(string_finger_defined)
	{	//If the finger definition was complete
		return 1;	//Return fingering valid
	}
	return 2;	//Return fingering undefined
}

void eof_song_fix_fingerings(EOF_SONG *sp, char *undo_made)
{
	unsigned long ctr;

	if(!sp)
		return;	//Invalid parameter

	for(ctr = 1; ctr < sp->tracks; ctr++)
	{	//For each track (skipping the NULL global track 0)
		if(sp->track[ctr]->track_format == EOF_PRO_GUITAR_TRACK_FORMAT)
		{	//If this is a pro guitar track
			eof_pro_guitar_track_fix_fingerings(sp->pro_guitar_track[sp->track[ctr]->tracknum], undo_made);	//Correct and complete note fingering where possible, performing an undo state before making changes
		}
	}
}

void eof_generate_efficient_hand_positions(EOF_SONG *sp, unsigned long track, char difficulty, char warnuser, char dynamic)
{
	unsigned long ctr, ctr2, tracknum, count, bitmask, beatctr, startpos = 0, endpos, shapenum;
	EOF_PRO_GUITAR_TRACK *tp;
	unsigned char current_low, current_high, last_anchor = 0;
	EOF_PRO_GUITAR_NOTE *next_position = NULL;	//Tracks the note at which the next fret hand position will be placed
	EOF_PRO_GUITAR_NOTE *np, temp;
	char force_change, started = 0;

	if(!sp || (track >= sp->tracks) || (sp->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT))
		return;	//Invalid parameters

	//Remove any existing fret hand positions defined for this track difficulty
	tracknum = sp->track[track]->tracknum;
	tp = sp->pro_guitar_track[tracknum];
	if(tp->notes == 0)
		return;	//Invalid parameters (track must have at least 1 note)
	for(ctr = tp->handpositions; ctr > 0; ctr--)
	{	//For each existing hand positions in this track (in reverse order)
		if(tp->handposition[ctr - 1].difficulty == difficulty)
		{	//If this hand position is defined for the specified difficulty
			if(warnuser)
			{
				eof_clear_input();
				key[KEY_Y] = 0;
				key[KEY_N] = 0;
				if(alert(NULL, "Existing fret hand positions for the active track difficulty will be removed.", "Continue?", "&Yes", "&No", 'y', 'n') != 1)
				{	//If the user does not opt to remove the existing hand positions
					return;
				}
			}
			warnuser = 0;
			if(!eof_fret_hand_position_list_dialog_undo_made)
			{	//If an undo state hasn't been made yet since launching this dialog
				eof_prepare_undo(EOF_UNDO_TYPE_NONE);
				eof_fret_hand_position_list_dialog_undo_made = 1;
			}
			eof_pro_guitar_track_delete_hand_position(tp, ctr - 1);	//Delete the hand position
		}
	}
	eof_pro_guitar_track_sort_fret_hand_positions(tp);	//Sort the positions

	//Count the number of notes in the specified track difficulty and allocate arrays large enough to store the lowest and highest fret number used in each
	for(ctr = 0, count = 0; ctr < tp->notes; ctr++)
	{	//For each note in the track
		if(tp->note[ctr]->type == difficulty)
		{	//If it is in the specified difficulty
			count++;	//Increment this counter
		}
	}

	if(!count)
	{	//If this track difficulty has no notes
		return;	//Exit function
	}

	eof_build_fret_range_tolerances(tp, difficulty, dynamic);	//Allocate and build eof_fret_range_tolerances[], using the calling function's chosen option regarding tolerances
	if(!eof_fret_range_tolerances)
	{	//eof_fret_range_tolerances[] wasn't built
		return;
	}
	if(!eof_fret_hand_position_list_dialog_undo_made)
	{	//If an undo hasn't been made yet, do it now as there will be at least one fret hand position added
		eof_prepare_undo(EOF_UNDO_TYPE_NONE);
	}

	//Iterate through this track difficulty's notes and determine efficient hand positions
	current_low = current_high = 0;	//Reset these at the start of generating hand positions
	for(ctr = 0; ctr < tp->notes; ctr++)
	{	//For each note in the track
		if((tp->note[ctr]->type == difficulty) && !(tp->note[ctr]->tflags & EOF_NOTE_TFLAG_TEMP))
		{	//If it is in the specified difficulty and isn't marked as a temporary note (a single note temporarily inserted to allow chord techniques to appear in Rocksmith 1)
			if(!next_position)
			{	//If this is the first note since the last hand position, or there was no hand position placed yet
				next_position = tp->note[ctr];	//Store its address
			}

			//Determine if this chord uses the index finger, which will trigger a fret hand position change (if this chord's fingering is incomplete, perform a chord shape lookup)
			force_change = 0;	//Reset this condition
			np = tp->note[ctr];	//Unless the chord's fingering is incomplete, the note's current fingering will be used to determine whether the index finger triggers a position change
			if(eof_pro_guitar_note_fingering_valid(tp, ctr) != 1)
			{	//If the fingering for the note is not fully defined
				if(eof_lookup_chord_shape(np, &shapenum, 0))
				{	//If a fingering for the chord can be found in the chord shape definitions
					memcpy(temp.frets, np->frets, 6);	//Clone the fretting of the original note into the temporary note
					temp.note = np->note;				//Clone the note mask
					eof_apply_chord_shape_definition(&temp, shapenum);	//Apply the matching chord shape definition's fingering to the temporary note
					np = &temp;	//Check the temporary note for use of the index finger, instead of the original note
				}
			}
			for(ctr2 = 0, bitmask = 1; ctr2 < 6; ctr2++, bitmask <<= 1)
			{	//For each of the 6 supported strings
				if((np->note & bitmask) && (np->finger[ctr2] == 1))
				{	//If this note uses this string, and the string is defined as being fretted by the index finger
					force_change = 1;
					break;
				}
			}

			if(force_change || !eof_note_can_be_played_within_fret_tolerance(tp, ctr, &current_low, &current_high))
			{	//If a position change was determined to be necessary based on fingering, or this note can't be included with previous notes within a single fret hand position
				if(current_low + tp->capo > 19)
				{	//Ensure the fret hand position (taking the capo position into account) is capped at 19, since 22 is the highest fret supported in either Rock Band or Rocksmith
					current_low = 19 - tp->capo;
				}
				if(force_change)
				{	//If a fret hand position change was forced due to note fingering
					if(next_position != tp->note[ctr])
					{	//If the fret hand position for previous notes has not been placed yet, write it first
						if(current_low && (current_low != last_anchor))
						{	//As long as the hand position being written is valid and is is different from the previous one
							(void) eof_track_add_section(sp, track, EOF_FRET_HAND_POS_SECTION, difficulty, next_position->pos, current_low, 0, NULL);	//Add the fret hand position for this forced position change
							last_anchor = current_low;
						}
						next_position = tp->note[ctr];	//The fret hand position for the current note will be written next
					}
					//Now that the previous notes' position is in place, update the high and low fret tracking for the current note
					current_low = eof_pro_guitar_note_lowest_fret(tp, ctr);	//Track this note's high and low frets
					current_high = eof_pro_guitar_note_highest_fret(tp, ctr);
					if(current_low && (current_low != last_anchor))
					{	//As long as the hand position being written is valid and is is different from the previous one
						(void) eof_track_add_section(sp, track, EOF_FRET_HAND_POS_SECTION, difficulty, next_position->pos, current_low, 0, NULL);	//Add the fret hand position for this forced position change
						last_anchor = current_low;
					}
					next_position = NULL;	//This note's position will not receive another hand position, the next loop iteration will look for any necessary position changes starting with the next note's location
				}
				else
				{	//If the position change is being placed on a previous note due to this note going out of fret tolerance
					if(current_low && (current_low != last_anchor))
					{	//As long as the hand position being written is valid and is is different from the previous one
						(void) eof_track_add_section(sp, track, EOF_FRET_HAND_POS_SECTION, difficulty, next_position->pos, current_low, 0, NULL);	//Add the fret hand position for this forced position change
						last_anchor = current_low;
					}
					next_position = tp->note[ctr];	//The fret hand position for the current note will be written next
				}
				current_low = eof_pro_guitar_note_lowest_fret(tp, ctr);	//Track this note's high and low frets
				current_high = eof_pro_guitar_note_highest_fret(tp, ctr);
			}//If a position change was determined to be necessary based on fingering, or this note can't be included with previous notes within a single fret hand position
		}//If it is in the specified difficulty and isn't marked as a temporary note (a single note inserted to allow chord techniques to appear in Rocksmith)
	}//For each note in the track

	//The last one or more notes examined need to have their hand position placed
	if(!current_low)
	{	//If only open notes were played in this track difficulty
		current_low = 1;	//Place the fret hand position at fret 1
	}
	else if(current_low + tp->capo > 19)
	{	//Ensure the fret hand position (taking the capo position into account) is capped at 19, since 22 is the highest fret supported in either Rock Band or Rocksmith
		current_low = 19 - tp->capo;
	}
	if((current_low != last_anchor) && next_position)
	{	//If the last parsed note requires a position change
		(void) eof_track_add_section(sp, track, EOF_FRET_HAND_POS_SECTION, difficulty, next_position->pos, current_low, 0, NULL);	//Add the best determined fret hand position
	}

	//Ensure that a fret hand position is defined in each phrase, at or before its first note
	for(beatctr = 0; beatctr < sp->beats; beatctr++)
	{	//For each beat in the project
		if((sp->beat[beatctr]->contained_section_event >= 0) || ((beatctr + 1 >= sp->beats) && started))
		{	//If this beat has a section event (RS phrase) or a phrase is in progress and this is the last beat, it marks the end of any current phrase and the potential start of another
			if(started)
			{	//If the first phrase marker has been encountered, this beat marks the end of a phrase
				endpos = sp->beat[beatctr]->pos - 1;	//Track this as the end position of the phrase
				(void) eof_enforce_rs_phrase_begin_with_fret_hand_position(sp, track, difficulty, startpos, endpos, &eof_fret_hand_position_list_dialog_undo_made, 0);	//Add a fret hand position
			}//If the first phrase marker has been encountered, this beat marks the end of a phrase

			started = 1;	//Track that a phrase has been encountered
			startpos = eof_song->beat[beatctr]->pos;	//Track the starting position of the phrase
		}//If this beat has a section event (RS phrase) or a phrase is in progress and this is the last beat, it marks the end of any current phrase and the potential start of another
	}//For each beat in the project

	//Clean up
	free(eof_fret_range_tolerances);
	eof_fret_range_tolerances = NULL;	//Clear this array so that the next call to eof_build_fret_range_tolerances() rebuilds it accordingly
	eof_pro_guitar_track_sort_fret_hand_positions(tp);	//Sort the positions
	eof_render();
}

int eof_generate_hand_positions_current_track_difficulty(void)
{
	int junk;
	unsigned long diffindex = 0;

	if(!eof_song || (eof_song->track[eof_selected_track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT))
		return 0;	//Invalid parameters

	eof_generate_efficient_hand_positions(eof_song, eof_selected_track, eof_note_type, 1, 0);	//Warn the user if existing hand positions will be deleted (use a static fret range tolerance of 4 for all frets, since it's assumed the author is charting for Rocksmith if they use this function)

	(void) eof_pro_guitar_track_find_effective_fret_hand_position_definition(eof_song->pro_guitar_track[eof_song->track[eof_selected_track]->tracknum], eof_note_type, eof_music_pos - eof_av_delay, NULL, &diffindex, 0);
		//Obtain the fret hand position change now in effect at the current seek position
	eof_fret_hand_position_list_dialog[1].d1 = diffindex;	//Pre-select it in the list
	(void) dialog_message(eof_fret_hand_position_list_dialog, MSG_START, 0, &junk);	//Re-initialize the dialog
	(void) dialog_message(eof_fret_hand_position_list_dialog, MSG_DRAW, 0, &junk);	//Redraw dialog
	return D_REDRAW;
}

int eof_note_can_be_played_within_fret_tolerance(EOF_PRO_GUITAR_TRACK *tp, unsigned long note, unsigned char *current_low, unsigned char *current_high)
{
	unsigned char effective_lowest, effective_highest;	//Stores the cumulative highest and lowest fret values with the input range and the next note for tolerance testing
	long next;

	if(!tp || !current_low || !current_high || (note >= tp->notes) || (*current_low > *current_high) || (*current_high > tp->numfrets) || !eof_fret_range_tolerances)
		return 0;	//Invalid parameters

	while(1)
	{
		effective_lowest = eof_pro_guitar_note_lowest_fret(tp, note);	//Find the highest and lowest fret used in the note
		effective_highest = eof_pro_guitar_note_highest_fret(tp, note);
		if(!effective_lowest && (note + 1 < tp->notes))
		{	//If only open strings are played in the note
			next = eof_fixup_next_pro_guitar_note(tp, note);	//Determine if there's another note in this track difficulty
			if(next >= 0)
			{	//If there was another note
				note = next;	//Examine it and recheck its highest and lowest frets (so that position changes can be made for further ahead notes)
				continue;
			}
		}
		break;
	}

	if(!(*current_low))
	{	//If there's no hand position in effect yet, init the currently tracked high and low frets with this note's
		*current_low = effective_lowest;
		*current_high = effective_highest;
		return 1;	//Return note can be played without an additional hand position
	}

	if(eof_pro_guitar_note_is_barre_chord(tp, note))
	{	//If this note is a barre chord
		if(*current_low != effective_lowest)
		{	//If the ongoing lowest fret value is not at lowest used fret in this barre chord (where the index finger will need to be to play the chord)
			return 0;	//Return note cannot be played without an additional hand position
		}
	}
	if(!effective_lowest)
	{	//If this note didn't have a low fret value
		effective_lowest = *current_low;	//Keep the ongoing lowest fret value
	}
	else if((*current_low != 0) && (*current_low < effective_lowest))
	{	//Otherwise keep the ongoing lowest fret value only if it is defined and is lower than this note's lowest fret value
		effective_lowest = *current_low;
	}
	if(*current_high > effective_highest)
	{	//Obtain the higher of these two values
		effective_highest = *current_high;
	}

	if(effective_highest - effective_lowest + 1 > eof_fret_range_tolerances[effective_lowest])
	{	//If this note can't be played at the same hand position as the one already in effect
		return 0;	//Return note cannot be played without an additional hand position
	}

	*current_low = effective_lowest;	//Update the effective highest and lowest used frets
	*current_high = effective_highest;
	return 1;	//Return note can be played without an additional hand position
}

void eof_build_fret_range_tolerances(EOF_PRO_GUITAR_TRACK *tp, unsigned char difficulty, char dynamic)
{
	unsigned long ctr;
	unsigned char lowest, highest, range;

	if(!tp)
	{	//Invalid parameters
		eof_fret_range_tolerances = NULL;
		return;
	}

	if(eof_fret_range_tolerances)
	{	//If this array was previously built
		free(eof_fret_range_tolerances);	//Release its memory as it will be rebuilt to suit this track difficulty
	}
	eof_fret_range_tolerances = malloc((size_t)tp->numfrets + 1);	//Allocate memory for an array large enough to specify the fret hand's range for each fret starting the numbering with 1 instead of 0
	if(!eof_fret_range_tolerances)
	{	//Couldn't allocate memory
		return;
	}

	//Initialize the tolerance of each fret to 4
	memset(eof_fret_range_tolerances, 4, (size_t)tp->numfrets + 1);	//Set a default range of 4 frets for the entire guitar neck

	if(!dynamic)
	{	//If the tolerances aren't being built from the specified track, return with all tolerances initialized to 4
		return;
	}

	//Find the range of each fret as per the notes in the track
	for(ctr = 0; ctr < tp->notes; ctr++)
	{	//For each note in the specified track
		if(tp->note[ctr]->type == difficulty)
		{	//If it is in the specified difficulty
			lowest = eof_pro_guitar_note_lowest_fret(tp, ctr);	//Get the lowest and highest fret used by this note
			highest = eof_pro_guitar_note_highest_fret(tp, ctr);

			range = highest - lowest + 1;	//Determine the range used by this note, and assume it's range of frets is playable since that's how the chord is defined for play
			if(eof_fret_range_tolerances[lowest] < range)
			{	//If the current fret range for this fret position is lower than this chord uses
				eof_fret_range_tolerances[lowest] = range;	//Update the range to reflect that this chord is playable
			}
		}
	}

	//Update the array so that any range that is valid for a lower fret number is valid for a higher fret number
	range = eof_fret_range_tolerances[1];	//Start with the range of fret 1
	for(ctr = 2; ctr < tp->numfrets + 1; ctr++)
	{	//For each of the frets in the array, starting with the second
		if(eof_fret_range_tolerances[ctr] < range)
		{	//If this fret's defined range is lower than a lower (longer fret)
			eof_fret_range_tolerances[ctr] = range;	//Update it
		}
		range = eof_fret_range_tolerances[ctr];	//Track the current fret's range
	}
}

unsigned char eof_pro_guitar_track_find_effective_fret_hand_position(EOF_PRO_GUITAR_TRACK *tp, unsigned char difficulty, unsigned long position)
{
	unsigned long ctr;
	unsigned char effective = 0;

	if(!tp)
		return 0;

	for(ctr = 0; ctr < tp->handpositions; ctr++)
	{	//For each hand position in the track
		if(tp->handposition[ctr].difficulty == difficulty)
		{	//If the hand position is in the specified difficulty
			if(tp->handposition[ctr].start_pos <= position)
			{	//If the hand position is at or before the specified timestamp
				effective = tp->handposition[ctr].end_pos;	//Track its fret number
			}
			else
			{	//This hand position is beyond the specified timestamp
				return effective;	//Return the last hand position that was found (if any) in this track difficulty
			}
		}
	}

	return effective;	//Return the last hand position definition that was found (if any) in this track difficulty
}

EOF_PHRASE_SECTION *eof_pro_guitar_track_find_effective_fret_hand_position_definition(EOF_PRO_GUITAR_TRACK *tp, unsigned char difficulty, unsigned long position, unsigned long *index, unsigned long *diffindex, char function)
{
	unsigned long ctr, ctr2 = 0;
	EOF_PHRASE_SECTION *ptr = NULL;

	if(!tp)
		return 0;	//Invalid parameters

	for(ctr = 0; ctr < tp->handpositions; ctr++)
	{	//For each hand position in the track
		if(tp->handposition[ctr].difficulty == difficulty)
		{	//If the hand position is in the specified difficulty
			if(tp->handposition[ctr].start_pos <= position)
			{	//If the hand position is at or before the specified timestamp
				if(function && (tp->handposition[ctr].start_pos != position))
				{	//If the calling function only wanted to identify the fret hand position exactly at the target time stamp
					continue;	//Skip to next position if this one isn't at the target time
				}
				ptr = &tp->handposition[ctr];	//Store its address
				if(index)
				{	//If index isn't NULL
					*index = ctr;		//Track this fret hand position definition number
				}
				if(diffindex)
				{	//If diffindex isn't NULL
					*diffindex = ctr2;	//Track the position number this is within the target difficulty
				}
			}
			else
			{	//This hand position is beyond the specified timestamp
				return ptr;		//Return any hand position address that has been found
			}
			ctr2++;
		}
	}

	return ptr;	//Return any hand position address that has been found
}

char *eof_rs_section_text_valid(char *string)
{
	unsigned long ctr;

	if(!string)
		return NULL;	//Return error

	for(ctr = 0; ctr < EOF_NUM_RS_PREDEFINED_SECTIONS; ctr++)
	{	//For each pre-defined Rocksmith section
		if(!ustricmp(eof_rs_predefined_sections[ctr].string, string) || !ustricmp(eof_rs_predefined_sections[ctr].displayname, string))
		{	//If the string matches this Rocksmith section entry's native or display name
			return eof_rs_predefined_sections[ctr].string;	//Return match
		}
	}
	return NULL;	//Return no match
}

int eof_rs_event_text_valid(char *string)
{
	unsigned long ctr;

	if(!string)
		return 0;	//Return error

	for(ctr = 0; ctr < EOF_NUM_RS_PREDEFINED_EVENTS; ctr++)
	{	//For each pre-defined Rocksmith event
		if(!ustrcmp(eof_rs_predefined_events[ctr].string, string))
		{	//If the string matches this Rocksmith event entry
			return 1;	//Return match
		}
	}
	return 0;	//Return no match
}

unsigned long eof_get_rs_section_instance_number(EOF_SONG *sp, unsigned long track, unsigned long event)
{
	unsigned long ctr, count = 1;

	if(!sp || (event >= sp->text_events) || !(sp->text_event[event]->flags & EOF_EVENT_FLAG_RS_SECTION))
		return 0;	//If the parameters are invalid, or the specified text event is not a Rocksmith section
	if(sp->text_event[event]->track && (sp->text_event[event]->track != track))
		return 0;	//If the specified event is assigned to a track other than the one specified

	for(ctr = 0; ctr < event; ctr++)
	{	//For each text event in the chart that is before the specified event
		if(sp->text_event[ctr]->flags & EOF_EVENT_FLAG_RS_SECTION)
		{	//If the text event is marked as a Rocksmith section
			if(!sp->text_event[ctr]->track || (sp->text_event[ctr]->track == track))
			{	//If the text event is not track specific or is assigned to the specified track
				if(!ustrcmp(sp->text_event[ctr]->text, sp->text_event[event]->text))
				{	//If the text event's text matches
					count++;	//Increment the instance counter
				}
			}
		}
	}

	return count;
}

void eof_get_rocksmith_wav_path(char *buffer, const char *parent_folder, size_t num)
{
	(void) replace_filename(buffer, parent_folder, "", (int)num - 1);	//Obtain the destination path

	//Build target WAV file name
	put_backslash(buffer);
	if(eof_song->tags->title[0] != '\0')
	{	//If the chart has a defined song title
		(void) ustrncat(buffer, eof_song->tags->title, (int)num - 1);
	}
	else
	{	//Otherwise default to "guitar"
		(void) ustrncat(buffer, "guitar", (int)num - 1);
	}
	(void) ustrncat(buffer, ".wav", (int)num - 1);
	buffer[num - 1] = '\0';	//Ensure the finalized string is terminated
}

void eof_delete_rocksmith_wav(void)
{
	char checkfn[1024] = {0};

	eof_get_rocksmith_wav_path(checkfn, eof_song_path, sizeof(checkfn));	//Build the path to the WAV file written for Rocksmith during save
	if(exists(checkfn))
	{	//If the path based on the song title exists
		(void) delete_file(checkfn);	//Delete it, if it exists, since changing the chart's OGG will necessitate rewriting the WAV file during save
	}
	else
	{	//Otherwise delete guitar.wav because this is the name it will use if the song title has characters invalid for a filename
		(void) replace_filename(checkfn, eof_song_path, "guitar.wav", 1024);
		(void) delete_file(checkfn);
	}
}

char eof_compare_time_range_with_previous_or_next_difficulty(EOF_SONG *sp, unsigned long track, unsigned long start, unsigned long stop, unsigned char diff, char compareto)
{
	unsigned long ctr2, ctr3, thispos, thispos2;
	unsigned char note_found;
	unsigned char comparediff, thisdiff, populated = 0;

	if(!sp || (track >= sp->tracks) || (start > stop))
		return 0;	//Invalid parameters
	if(!diff && (compareto < 0))
		return 0;	//There is no difficulty before the first difficulty
	if((diff == 255) && (compareto >= 0))
		return 0;	//There is no difficulty after the last difficulty

	if(compareto < 0)
	{	//Compare the specified difficulty with the previous difficulty
		if(!diff)
			return 0;	//There is no difficulty before the first difficulty
		comparediff = diff - 1;
		if(((sp->track[track]->flags & EOF_TRACK_FLAG_UNLIMITED_DIFFS) == 0) && (comparediff == 4))
		{	//If the track is using the traditional 5 difficulty system and the difficulty previous to the being examined is the BRE difficulty
			comparediff--;	//Compare to the previous difficulty
		}
	}
	else
	{	//Compare the specified difficulty with the next difficulty
		if(diff == 255)
			return 0;	//There is no difficulty after the last difficulty
		comparediff = diff + 1;
		if(((sp->track[track]->flags & EOF_TRACK_FLAG_UNLIMITED_DIFFS) == 0) && (comparediff == 4))
		{	//If the track is using the traditional 5 difficulty system and the difficulty next to the being examined is the BRE difficulty
			comparediff++;	//Compare to the next difficulty
		}
	}

	//First pass:  Compare notes in the specified difficulty with those in the comparing difficulty
	for(ctr2 = 0; ctr2 < eof_get_track_size(sp, track); ctr2++)
	{	//For each note in the track
		thispos = eof_get_note_pos(sp, track, ctr2);	//Get this note's position
		if(thispos > stop)
		{	//If this note (and all remaining notes, since they are expected to remain sorted) is beyond the specified range, break from loop
			break;
		}

		if(thispos >= start)
		{	//If this note is at or after the start of the specified range, check its difficulty
			thisdiff = eof_get_note_type(sp, track, ctr2);	//Get this note's difficulty
			if(thisdiff == diff)
			{	//If this note is in the difficulty being examined
				populated = 1;	//Track that there was at least 1 note in the specified difficulty of the phrase
				//Compare this note to the one at the same position in the comparing difficulty, if there is one
				note_found = 0;	//This condition will be set if this note is found in the comparing difficulty
				if(compareto < 0)
				{	//If comparing to the previous difficulty, parse notes backwards
					for(ctr3 = ctr2; ctr3 > 0; ctr3--)
					{	//For each previous note in the track, for performance reasons going backward from the one being examined in the outer loop (which has a higher note number when sorted)
						thispos2 = eof_get_note_pos(sp, track, ctr3 - 1);	//Get this note's position
						thisdiff = eof_get_note_type(sp, track, ctr3 - 1);	//Get this note's difficulty
						if(thispos2 < thispos)
						{	//If this note and all previous ones are before the one being examined in the outer loop
							break;
						}
						if((thispos == thispos2) && (thisdiff == comparediff))
						{	//If this note is at the same position and one difficulty lower than the one being examined in the outer loop
							note_found = 1;	//Track that a note at the same position was found in the previous difficulty
							if(eof_note_compare(sp, track, ctr2, track, ctr3 - 1, 1))
							{	//If the two notes don't match (including lengths and flags)
								return 1;	//Return difference found
							}
							break;
						}
					}
				}
				else
				{	//Comparing to the next difficulty, parse notes forwards
					for(ctr3 = ctr2 + 1; ctr3 < eof_get_track_size(sp, track); ctr3++)
					{	//For each remaining note in the track
						thispos2 = eof_get_note_pos(sp, track, ctr3);	//Get this note's position
						thisdiff = eof_get_note_type(sp, track, ctr3);	//Get this note's difficulty
						if(thispos2 > thispos)
						{	//If this note and all subsequent ones are after the one being examined in the outer loop
							break;
						}
						if((thispos == thispos2) && (thisdiff == comparediff))
						{	//If this note is at the same position and one difficulty lower than the one being examined in the outer loop
							note_found = 1;	//Track that a note at the same position was found in the previous difficulty
							if(eof_note_compare(sp, track, ctr2, track, ctr3 - 1, 1))
							{	//If the two notes don't match (including lengths and flags)
								return 1;	//Return difference found
							}
							break;
						}
					}
				}
				if(!note_found)
				{	//If this note has no note at the same position in the previous difficulty
					return 1;	//Return difference found
				}
			}//If this note is in the difficulty being examined
		}//If this note is at or after the start of the specified range, check its difficulty
	}//For each note in the track

	if(!populated)
	{	//If no notes were contained within the time range in the specified difficulty
		return -1;	//Return empty time range
	}

	//Second pass:  Compare notes in the comparing difficulty with those in the specified difficulty
	for(ctr2 = 0; ctr2 < eof_get_track_size(sp, track); ctr2++)
	{	//For each note in the track
		thispos = eof_get_note_pos(sp, track, ctr2);	//Get this note's position
		if(thispos > stop)
		{	//If this note (and all remaining notes, since they are expected to remain sorted) is beyond the specified range, break from loop
			break;
		}

		if(thispos >= start)
		{	//If this note is at or after the start of the specified range, check its difficulty
			thisdiff = eof_get_note_type(sp, track, ctr2);	//Get this note's difficulty
			if(thisdiff == comparediff)
			{	//If this note is in the difficulty being compared
				//Compare this note to the one at the same position in the examined difficulty, if there is one
				note_found = 0;	//This condition will be set if this note is found in the examined difficulty
				if(compareto < 0)
				{	//If comparing the comparing difficulty with the next, parse notes forwards
					for(ctr3 = ctr2 + 1; ctr3 < eof_get_track_size(sp, track); ctr3++)
					{	//For each remaining note in the track
						thispos2 = eof_get_note_pos(sp, track, ctr3);	//Get this note's position
						thisdiff = eof_get_note_type(sp, track, ctr3);	//Get this note's difficulty
						if(thispos2 > thispos)
						{	//If this note and all subsequent ones are after the one being examined in the outer loop
							break;
						}
						if((thispos == thispos2) && (thisdiff == diff))
						{	//If this note is in the specified difficulty and at the same position as the one being examined in the outer loop
							note_found = 1;	//Track that a note at the same position was found in the previous difficulty
							if(eof_note_compare(sp, track, ctr2, track, ctr3 - 1, 1))
							{	//If the two notes don't match (including lengths and flags)
								return 1;	//Return difference found
							}
							break;
						}
					}
				}
				else
				{	//Comparing the comparing difficulty with the previous, parse notes backwards
					for(ctr3 = ctr2; ctr3 > 0; ctr3--)
					{	//For each previous note in the track, for performance reasons going backward from the one being examined in the outer loop (which has a higher note number when sorted)
						thispos2 = eof_get_note_pos(sp, track, ctr3 - 1);	//Get this note's position
						thisdiff = eof_get_note_type(sp, track, ctr3 - 1);	//Get this note's difficulty
						if(thispos2 < thispos)
						{	//If this note and all previous ones are before the one being examined in the outer loop
							break;
						}
						if((thispos == thispos2) && (thisdiff == diff))
						{	//If this note is in the specified difficulty and at the same position as the one being examined in the outer loop
							note_found = 1;	//Track that a note at the same position was found in the previous difficulty
							if(eof_note_compare(sp, track, ctr2, track, ctr3 - 1, 1))
							{	//If the two notes don't match (including lengths and flags)
								return 1;	//Return difference found
							}
							break;
						}
					}
				}
				if(!note_found)
				{	//If this note has no note at the same position in the previous difficulty
					return 1;	//Return difference found
				}
			}//If this note is in the difficulty being compared
		}//If this note is at or after the start of the specified range, check its difficulty
	}//For each note in the track

	return 0;	//Return no difference found
}

unsigned char eof_find_fully_leveled_rs_difficulty_in_time_range(EOF_SONG *sp, unsigned long track, unsigned long start, unsigned long stop, unsigned char relative)
{
	unsigned char reldiff, fullyleveleddiff = 0;
	unsigned long ctr;

	if(!sp || (track >= sp->tracks) || (start > stop))
		return 0;	//Invalid parameters

	(void) eof_detect_difficulties(sp, track);	//Update eof_track_diff_populated_status[] to reflect all populated difficulties for this track
	if((sp->track[track]->flags & EOF_TRACK_FLAG_UNLIMITED_DIFFS) == 0)
	{	//If the track is using the traditional 5 difficulty system
		eof_track_diff_populated_status[4] = 0;	//Ensure that the BRE difficulty is ignored
	}
	for(ctr = 0; ctr < 256; ctr++)
	{	//For each of the possible difficulties
		if(eof_track_diff_populated_status[ctr])
		{	//If this difficulty isn't empty
			if(eof_compare_time_range_with_previous_or_next_difficulty(sp, track, start, stop, ctr, -1) > 0)
			{	//If this difficulty isn't empty and had more notes than the previous or any of the notes within the phrase were different than those in the previous difficulty
				fullyleveleddiff = ctr;			//Track the lowest difficulty number that represents the fully leveled time range of notes
			}
		}
	}//For each of the possible difficulties

	if(!relative)
	{	//If the resulting difficulty number is not to be converted to Rocksmith's relative difficulty number system
		return fullyleveleddiff;
	}

	//Remap fullyleveleddiff to the relative difficulty
	for(ctr = 0, reldiff = 0; ctr < 256; ctr++)
	{	//For each of the possible difficulties
		if(((sp->track[track]->flags & EOF_TRACK_FLAG_UNLIMITED_DIFFS) == 0) && (ctr == 4))
		{	//If the track is using the traditional 5 difficulty system and the difficulty being examined is the BRE difficulty
			continue;	//Don't process the BRE difficulty, it will not be exported
		}
		if(fullyleveleddiff == ctr)
		{	//If the corresponding relative difficulty has been found
			return reldiff;	//Return it
		}
		if(eof_track_diff_populated_status[ctr])
		{	//If the track is populated
			reldiff++;
		}
	}
	return 0;	//Error
}

int eof_check_rs_sections_have_phrases(EOF_SONG *sp, unsigned long track)
{
	unsigned long ctr, old_text_events;
	char user_prompted = 0;

	if(!sp || (track >= sp->tracks))
		return 1;	//Invalid parameters
	if(!eof_get_track_size(sp, track))
		return 0;	//Empty track

	eof_process_beat_statistics(sp, track);	//Cache section name information into the beat structures (from the perspective of the specified track)
	for(ctr = 0; ctr < sp->beats; ctr++)
	{	//For each beat
		if(sp->beat[ctr]->contained_rs_section_event >= 0)
		{	//If this beat contains a RS section
			if(sp->beat[ctr]->contained_section_event < 0)
			{	//But it doesn't contain a RS phrase
				eof_selected_beat = ctr;					//Change the selected beat
				if(eof_2d_render_top_option != 36)
				{	//If the piano roll isn't already displaying both RS sections and phrases
					eof_2d_render_top_option = 35;					//Change the user preference to render RS sections at the top of the piano roll
				}
				eof_seek_and_render_position(track, eof_note_type, sp->beat[ctr]->pos);	//Render the track so the user can see where the correction needs to be made, along with the RS section in question
				eof_clear_input();
				key[KEY_Y] = 0;
				key[KEY_N] = 0;
				if(!user_prompted && alert("At least one Rocksmith section doesn't have a Rocksmith phrase at the same position.", "This can cause the chart's sections to work incorrectly", "Would you like to place Rocksmith phrases to correct this?", "&Yes", "&No", 'y', 'n') != 1)
				{	//If the user hasn't already answered this prompt, and doesn't opt to correct the issue
					return 2;	//Return user cancellation
				}
				user_prompted = 1;

				while(1)
				{
					old_text_events = sp->text_events;				//Remember how many text events there were before launching dialog
					(void) eof_rocksmith_phrase_dialog_add();			//Launch dialog for user to add a Rocksmith phrase
					if(old_text_events == sp->text_events)
					{	//If the number of text events defined in the chart didn't change, the user canceled
						return 2;	//Return user cancellation
					}
					eof_process_beat_statistics(sp, track);	//Rebuild beat statistics to check if user added a Rocksmith phrase
					if(sp->beat[ctr]->contained_section_event < 0)
					{	//If user added a text event, but it wasn't a Rocksmith phrase
						eof_clear_input();
						key[KEY_Y] = 0;
						key[KEY_N] = 0;
						if(alert("You didn't add a Rocksmith phrase.", NULL, "Do you want to continue adding RS phrases for RS sections that are missing them?", "&Yes", "&No", 'y', 'n') != 1)
						{	//If the user doesn't opt to finish correcting the issue
							return 2;	//Return user cancellation
						}
					}
					else
					{	//User added the missing RS phrase
						break;
					}
				}
			}
		}
	}

	return 0;	//Return completion
}

unsigned long eof_find_effective_rs_phrase(unsigned long position)
{
	unsigned long ctr, effective = 0;

	for(ctr = 0; ctr < eof_song->beats; ctr++)
	{	//For each beat in the chart
		if(eof_song->beat[ctr]->contained_section_event >= 0)
		{	//If this beat has a section event (RS phrase)
			if(eof_song->beat[ctr]->pos <= position)
			{	//If this phrase is at or before the position being checked
				effective++;
			}
			else
			{	//If this phrase is after the position being checked
				break;
			}
		}
	}

	if(effective)
	{	//If the specified phrase was found
		return effective - 1;	//Return its number
	}

	return 0;	//Return no phrase found
}

int eof_time_range_is_populated(EOF_SONG *sp, unsigned long track, unsigned long start, unsigned long stop, unsigned char diff)
{
	unsigned long ctr2, thispos;
	unsigned char thisdiff;

	if(!sp || (track >= sp->tracks) || (start > stop))
		return 0;	//Invalid parameters

	for(ctr2 = 0; ctr2 < eof_get_track_size(sp, track); ctr2++)
	{	//For each note in the track
		thispos = eof_get_note_pos(sp, track, ctr2);	//Get this note's position
		if(thispos > stop)
		{	//If this note (and all remaining notes, since they are expected to remain sorted) is beyond the specified range, break from loop
			break;
		}
		if(thispos >= start)
		{	//If this note is at or after the start of the specified range, check its difficulty
			thisdiff = eof_get_note_type(sp, track, ctr2);	//Get this note's difficulty
			if(thisdiff == diff)
			{	//If this note is in the difficulty being examined
				return 1;	//Return specified range at the specified difficulty is populated
			}
		}
	}

	return 0;	//Return not populated
}

int eof_note_has_high_chord_density(EOF_SONG *sp, unsigned long track, unsigned long note, char target)
{
	long prev;
	EOF_RS_TECHNIQUES tech;

	if((sp == NULL) || (track >= sp->tracks))
		return 0;	//Error

	if(sp->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT)
		return 0;	//Note is not a pro guitar/bass note

	if(eof_get_note_flags(sp, track, note) & EOF_NOTE_FLAG_CRAZY)
		return 0;	//Note is marked with crazy status, which forces it to export as low density

	if(eof_note_count_rs_lanes(sp, track, note, target) < 2)
		return 0;	//Note is not a chord

	prev = eof_track_fixup_previous_note(sp, track, note);
	if(prev < 0)
		return 0;	//No earlier note

	if(eof_get_note_pos(sp, track, note) > eof_get_note_pos(sp, track, prev) + 10000)
		return 0;	//Note is not within 10000ms of the previous note

	if(eof_note_compare(sp, track, note, track, prev, 0))
		return 0;	//Note does not match the previous note (ignoring note flags and lengths)

	if(target == 2)
	{	//Additional checks for Rocksmith 2
		if(eof_get_rs_techniques(sp, track, note, 0, NULL, 2))
			return 0;	//Chord has one or more techniques that require it to be written as low density

		eof_get_rs_techniques(sp, track, prev, 0, &tech, 2);	//Get techniques used by the previous note
		if((eof_note_count_rs_lanes(sp, track, note, target) > 1) && (tech.slideto >= 0))
		{	//If the previous note was a chord slide
			return 0;
		}
	}

	return 1;	//All criteria passed, note is high density
}

int eof_enforce_rs_phrase_begin_with_fret_hand_position(EOF_SONG *sp, unsigned long track, unsigned char diff, unsigned long startpos, unsigned long endpos, char *undo_made, char check_only)
{
	unsigned long ctr, firstnotepos, tracknum;
	int found = 0;
	unsigned char position;
	EOF_PRO_GUITAR_TRACK *tp;

	if(!sp || (track >= sp->tracks) || (sp->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT))
		return 0;	//Invalid parameters
	tracknum = sp->track[track]->tracknum;
	tp = sp->pro_guitar_track[tracknum];

	//Find the position of the first note in the phrase
	found = 0;	//Reset this condition
	for(ctr = 0; ctr < tp->notes; ctr++)
	{	//For each note in the track
		if(tp->note[ctr]->type == diff)
		{	//If the note is in the active difficulty
			if((tp->note[ctr]->pos >= startpos) && (tp->note[ctr]->pos <= endpos))
			{	//If this is the first note in the phrase
				firstnotepos = tp->note[ctr]->pos;	//Note the position
				found = 1;
				break;
			}
		}
	}

	//Determine if the necessary fret hand position was set between the start of the phrase and the first note
	if(found)
	{	//If the phrase has a note in it
		found = 0;	//Reset this condition
		for(ctr = 0; ctr < tp->handpositions; ctr++)
		{	//For each hand position in the track
			if(tp->handposition[ctr].difficulty == diff)
			{	//If the hand position is in the active difficulty
				if((tp->handposition[ctr].start_pos >= startpos) && (tp->handposition[ctr].start_pos <= firstnotepos))
				{	//If the hand position is defined anywhere between the start of the phrase and the start of the first note in that phrase
					found = 1;
					break;
				}
			}
		}
		if(!found && !check_only)
		{	//If a hand position needs to be added to the difficulty, and the calling function intends for the position to be added
			position = eof_pro_guitar_track_find_effective_fret_hand_position(tp, diff, firstnotepos);
			if(position)
			{	//If a fret hand position was is in effect (placed anywhere earlier in the difficulty)
				if(undo_made && !(*undo_made))
				{	//If an undo state needs to be made
					eof_prepare_undo(EOF_UNDO_TYPE_NONE);
					*undo_made = 1;
				}
				//Place that fret hand position for the active difficulty
				(void) eof_track_add_section(sp, track, EOF_FRET_HAND_POS_SECTION, diff, firstnotepos, position, 0, NULL);
				eof_pro_guitar_track_sort_fret_hand_positions(tp);	//Sort the positions
			}
		}
	}

	return found;
}

int eof_lookup_chord_shape(EOF_PRO_GUITAR_NOTE *np, unsigned long *shapenum, unsigned long skipctr)
{
	unsigned long ctr, ctr2, bitmask;
	EOF_PRO_GUITAR_NOTE template;
	unsigned char lowest = 0;	//Tracks the lowest fret value in the note
	char nonmatch;

	if(!np)
		return 0;	//Invalid parameter

	//Prepare a copy of the target note that ignores open and muted strings and moves the note's shape to the lowest fret position and so that its lowest fretted string is lane 1
	//This will be used to easily compare against the chord shape definitions, which have been transformed the same way
	template = *np;
	for(ctr = 0, ctr2 = 0, bitmask = 1; ctr < 6; ctr++, bitmask <<= 1)
	{	//For each of the 6 supported strings
		if(template.note & bitmask)
		{	//If this string is used
			if((template.frets[ctr] == 0) || (template.frets[ctr] & 0x80))
			{	//If this string is played open or muted
				template.note &= ~bitmask;	//Clear this string on the template note
			}
			else
			{	//Otherwise the string is fretted
				ctr2++;	//Count how many strings are fretted
				if(!lowest || (template.frets[ctr] < lowest))
				{
					lowest = template.frets[ctr];	//Track the lowest fret value in the note
				}
			}
		}
	}
	if(ctr2 < 1)
	{	//If no strings are fretted, there cannot be a chord shape
		return 0;
	}
	for(ctr = 0; ctr < 6; ctr++)
	{	//For each of the 6 supported strings, lower the fretted notes by the same amount so shape is moved to fret 1
		if(template.frets[ctr] != 0)
		{	//If this note is fretted
			template.frets[ctr] -= (lowest - 1);	//Transpose the shape to the first fret
		}
	}
	while((template.note & 1) == 0)
	{	//Until the shape has been moved to occupy the lowest string
		for(ctr = 0; ctr < 5; ctr++)
		{	//For each of the first 5 supported strings
			template.frets[ctr] = template.frets[ctr + 1];		//Transpose the fretted note down one string
			template.finger[ctr] = template.finger[ctr + 1];	//Transpose the finger definition for the string
		}
		template.frets[5] = 0;
		template.finger[5] = 0;
		template.note >>= 1;	//Transpose the note mask
	}

	//Look for a chord shape definition that matches the template on any string position
	for(ctr = 0; ctr < num_eof_chord_shapes; ctr++)
	{	//For each chord shape definition
		//Compare the working copy note against the chord shape definition
		if(template.note == eof_chord_shape[ctr].note)
		{	//If the note bitmask matches that of the chord shape definition
			nonmatch = 0;	//Reset this status
			for(ctr2 = 0, bitmask = 1; ctr2 < 6; ctr2++, bitmask <<= 1)
			{	//For each of the 6 supported strings
				if(template.note & bitmask)
				{	//If this string is fretted
					if(template.frets[ctr2] != eof_chord_shape[ctr].frets[ctr2])
					{	//If the fret value doesn't match the chord shape definition
						nonmatch = 1;
						break;
					}
				}
			}
			if(!nonmatch)
			{	//If the note matches the chord shape definition
				if(!skipctr)
				{	//If no more matches were to be skipped before returning the match
					if(shapenum)
					{	//If calling function wanted to receive the matching definition number
						*shapenum = ctr;
					}
					return 1;	//Return match found
				}
				skipctr--;
			}
		}//If the note bitmask matches that of the chord shape definition
	}//For each chord shape definition

	return 0;	//No chord shape match found
}

void eof_apply_chord_shape_definition(EOF_PRO_GUITAR_NOTE *np, unsigned long shapenum)
{
	unsigned long ctr, transpose, bitmask;

	if(!np || (shapenum >= num_eof_chord_shapes))
		return;	//Invalid parameters

	//Find the lowest fretted string in the target note, this is where the chord shape begins
	//The specified chord shape definition will be transposed the appropriate number of strings
	for(transpose = 0, bitmask = 1; transpose < 6; transpose++, bitmask <<= 1)
	{	//For each of the 6 supported strings
		np->finger[transpose] = 0;	//Erase any existing fingering for this string
		if(np->note & bitmask)
		{	//If this string is used
			if((np->frets[transpose] != 0) && !(np->frets[transpose] & 0x80))
			{	//If this string is not played open or muted
				break;	//This string has the lowest fretted note
			}
		}
	}
	if(transpose >= 6)
		return;	//Error

	//Apply the chord shape's fingering, transosing the definition by the number of strings found necessary in the prior loop
	for(ctr = 0; ctr + transpose < 6; ctr++)
	{	//For the remainder of the 6 strings after any needed transposing is accounted for
		np->finger[ctr + transpose] = eof_chord_shape[shapenum].finger[ctr];	//Apply the defined fingering
	}
}

unsigned long eof_count_chord_shape_matches(EOF_PRO_GUITAR_NOTE *np)
{
	unsigned long ctr = 0;

	if(!np)
		return 0;	//Invalid parameter

	while(eof_lookup_chord_shape(np, NULL, ctr))
	{	//While another chord shape definition match is found
		ctr++;	//Try to find another
	}
	return ctr;
}

void eof_load_chord_shape_definitions(char *fn)
{
	char *buffer = NULL;	//Will be an array large enough to hold the largest line of text from input file
	PACKFILE *inf = NULL;
	size_t maxlinelength, length;
	unsigned long linectr = 1, ctr, bitmask;
	char finger[8] = {0};
	char frets[8] = {0};
	char name[51] = {0};
	unsigned char note, lowestfret;
	char error = 0;

	eof_log("\tImporting chord shape definitions", 1);
	eof_log("eof_load_chord_shape_definitions() entered", 1);

	if(!fn)
		return;	//Invalid parameter

	inf = pack_fopen(fn, "rt");	//Open file in text mode
	if(!inf)
	{
		(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tError loading:  Cannot open input chord shape definitions file:  \"%s\"", strerror(errno));	//Get the Operating System's reason for the failure
		eof_log(eof_log_string, 1);
		return;
	}

	//Allocate memory buffers large enough to hold any line in this file
	maxlinelength = (size_t)FindLongestLineLength_ALLEGRO(fn, 0);
	if(!maxlinelength)
	{
		eof_log("\tError finding the largest line in the file.  Aborting", 1);
		(void) pack_fclose(inf);
		return;
	}
	buffer = (char *)malloc(maxlinelength);
	if(!buffer)
	{
		eof_log("\tError allocating memory.  Aborting", 1);
		(void) pack_fclose(inf);
		return;
	}

	//Read first line of text, capping it to prevent buffer overflow
	if(!pack_fgets(buffer, (int)maxlinelength, inf))
	{	//I/O error
		(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "Chord shape definitions import failed on line #%lu:  Unable to read from file:  \"%s\"", linectr, strerror(errno));
		eof_log(eof_log_string, 1);
		error = 1;
	}

	//Parse the contents of the file
	while(!error)
	{	//Until there was an error reading from the file
		if(num_eof_chord_shapes < EOF_MAX_CHORD_SHAPES)
		{	//If another chord shape definition can be stored
			(void) snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tProcessing line #%lu", linectr);
			eof_log(eof_log_string, 1);

			//Load chord shape definition
			if(strcasestr_spec(buffer, "<chordTemplate"))
			{	//If this line contains a chord template tag (which defines a chord shape)
				if(eof_parse_chord_template(name, sizeof(name), finger, frets, &note, NULL, linectr, buffer))
				{	//If there was an error reading the chord template
					error = 1;
				}

				if(eof_note_count_colors_bitmask(note) < 2)
				{	//If not at least two strings are used in the definition
					eof_log("\t\tSkipping non chord definition", 1);
				}
				else
				{	//The chord shape is valid
					//Move the shape so that it begins on fret 1 and its lowest fretted string is lane 1
					for(ctr = 0, bitmask = 1, lowestfret = 0; ctr < 6; ctr++, bitmask <<= 1)
					{	//For each of the 6 supported strings
						if(note & bitmask)
						{	//If this string is used
							if(frets[ctr] == 0)
							{	//If this string is played open
								note &= ~bitmask;	//Clear this string from the note mask
							}
							else if(!lowestfret || (frets[ctr] < lowestfret))
							{
								lowestfret = frets[ctr];	//Track the lowest fret value in the note
							}
						}
					}
					for(ctr = 0; ctr < 6; ctr++)
					{	//For each of the 6 supported strings
						if(frets[ctr] >= lowestfret)
						{
							frets[ctr] -= (lowestfret - 1);	//Transpose any fretted strings to the first fret
						}
					}
					while((note & 1) == 0)
					{	//Until the shape has been moved to occupy the lowest string
						for(ctr = 0; ctr < 5; ctr++)
						{	//For each of the first 5 supported strings
							frets[ctr] = frets[ctr + 1];	//Transpose the fretted note down one string
							finger[ctr] = finger[ctr + 1];	//Transpose the finger definition for the string
						}
						frets[5] = 0;
						finger[5] = 0;
						note >>= 1;	//Transpose the note mask
					}

					//Add to list
					length = strlen(name);
					eof_chord_shape[num_eof_chord_shapes].name = malloc(length + 1);	//Allocate memory to store the shape name
					if(!eof_chord_shape[num_eof_chord_shapes].name)
					{
						eof_log("\tError allocating memory.  Aborting", 1);
						error = 1;
					}
					else
					{	//Memory was allocated
						memset(eof_chord_shape[num_eof_chord_shapes].name, 0, length + 1);	//Initialize memory block to 0
						strncpy(eof_chord_shape[num_eof_chord_shapes].name, name, strlen(name) + 1);
						memcpy(eof_chord_shape[num_eof_chord_shapes].finger, finger, 8);	//Store the finger array
						memcpy(eof_chord_shape[num_eof_chord_shapes].frets, frets, 8);		//Store the fret array
						eof_chord_shape[num_eof_chord_shapes].note = note;			//Store the note mask
						num_eof_chord_shapes++;
						eof_log("\t\tChord shape definition loaded", 1);
					}
				}//The chord shape is valid
			}//If this line contains a chord template tag (which defines a chord shape)
		}//If another chord shape definition can be stored
		else
		{	//The chord shape definition list is full
			error = 1;
		}

		//Use this method of checking for EOF instead of pack_feof(), otherwise the last line cannot be read and the definitions file doesn't use a closing tag that can be ignored
		if(!pack_fgets(buffer, (int)maxlinelength, inf))
		{	//If another line cannot be read from the file
			break;	//Exit loop
		}
		linectr++;	//Increment line counter
	}//Until there was an error reading from the file

	if(error)
	{	//If import did not complete successfully
		eof_destroy_shape_definitions();	//Destroy any imported shapes
	}

	//Cleanup
	(void) pack_fclose(inf);
	free(buffer);
}

void eof_destroy_shape_definitions(void)
{
	unsigned long ctr;

	for(ctr = 0; ctr < num_eof_chord_shapes; ctr++)
	{	//For each chord shape that was imported
		free(eof_chord_shape[ctr].name);	//Release the memory allocated to store the name
		eof_chord_shape[ctr].name = NULL;
	}
	num_eof_chord_shapes = 0;
}

void eof_export_rocksmith_showlights(EOF_SONG * sp, char * fn, unsigned long track)
{
	unsigned long ctr, ctr2, count, bitmask;
	int note;
	EOF_PRO_GUITAR_TRACK *tp;
	PACKFILE * fp;
	char buffer[50] = {0};

	eof_log("eof_export_rocksmith_showlights() entered", 1);

	if(!sp || !fn || (track >= sp->tracks) || (sp->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT))
	{
		eof_log("\tError saving:  Invalid parameters", 1);
		return;	//Return failure
	}

	fp = pack_fopen(fn, "w");
	if(!fp)
	{
		eof_log("\tError saving:  Cannot open file for writing", 1);
		return;	//Return failure
	}
	(void) pack_fputs("<?xml version='1.0' encoding='UTF-8'?>\n", fp);

	//Count how many entries will be exported
	eof_track_sort_notes(sp, track);	//Ensure the track is sorted
	tp = sp->pro_guitar_track[sp->track[track]->tracknum];
	for(ctr = 0, count = 0; ctr < tp->notes; ctr++)
	{	//For each note in the track
		//Find the next note to export
		if((ctr + 1 < tp->notes) && (tp->note[ctr]->pos == tp->note[ctr + 1]->pos))
		{	//If there is another note in the track, and it's in the same position
			continue;	//Skip this one and use the higher difficulty note
		}
		count++;	//This is the highest difficulty note at this position
	}
	(void) snprintf(buffer, sizeof(buffer) - 1, "<showlights count=\"%lu\">\n", count);
	(void) pack_fputs(buffer, fp);

	for(ctr = 0; ctr < tp->notes; ctr++)
	{	//For each note in the track
		//Find the next note to export
		if((ctr + 1 < tp->notes) && (tp->note[ctr]->pos == tp->note[ctr + 1]->pos))
		{	//If there is another note in the track, and it's in the same position
			continue;	//Skip this one and use the higher difficulty note
		}

		//Determine what MIDI note to export
		//Determine what string plays the lowest note in this single note/chord
		for(ctr2 = 0, bitmask = 1; ctr2 < 6; ctr2++, bitmask <<= 1)
		{	//For each of the 6 usable strings
			if(tp->note[ctr]->note & bitmask)
			{	//If this is the first populated string
				break;
			}
		}
		note = eof_lookup_default_string_tuning_absolute(tp, track, ctr2);	//Look up the default tuning for this string
		note += tp->tuning[ctr2];						//Account for the string's defined tuning
		if(tp->note[ctr]->frets[ctr2] != 0xFF)
		{	//If the fret value of this string is defined
			note += (tp->note[ctr]->frets[ctr2] & 0x7F);	//Account for what fret is being held on this string (mask out the string mute flag)
		}

		//Export the entry
		(void) snprintf(buffer, sizeof(buffer) - 1, "  <showlight time=\"%.3f\" note=\"%d\"/>\n", (double)tp->note[ctr]->pos / 1000.0, note);
		(void) pack_fputs(buffer, fp);
	}//For each note in the track

	(void) pack_fputs("</showlights>\n", fp);
	(void) pack_fclose(fp);
}

unsigned long eof_get_highest_fret_in_time_range(EOF_SONG *sp, unsigned long track, unsigned char difficulty, unsigned long start, unsigned long stop)
{
	unsigned long highest = 0, temp, ctr, tracknum;
	EOF_PRO_GUITAR_TRACK *tp;

	if(!sp || (track >= sp->tracks) || (sp->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT) || (stop < start))
		return 0;	//Invalid parameters

	tracknum = sp->track[track]->tracknum;
	tp = sp->pro_guitar_track[tracknum];
	for(ctr = 0; ctr < tp->notes; ctr++)
	{	//For each note in the track
		if((tp->note[ctr]->pos >= start) && (tp->note[ctr]->pos <= stop))
		{	//If the note is within the specified time range
			if(tp->note[ctr]->type == difficulty)
			{	//If the note is in the specified track difficulty
				temp = eof_get_highest_fret_value(sp, track, ctr);	//Determine its highest used fret
				if(temp > highest)
				{	//If it's the highest fret encountered so far
					highest = temp;	//Track this value
				}
			}
		}
		else if(tp->note[ctr]->pos > stop)
		{	//If this and all remaining notes are beyond the specified time range
			break;
		}
	}

	return highest;
}

unsigned long eof_get_rs_techniques(EOF_SONG *sp, unsigned long track, unsigned long notenum, unsigned long stringnum, EOF_RS_TECHNIQUES *ptr, char target)
{
	unsigned long tracknum, flags, fret;
	EOF_PRO_GUITAR_TRACK *tp;

	if((sp == NULL) || (track >= sp->tracks) || (sp->track[track]->track_format != EOF_PRO_GUITAR_TRACK_FORMAT))
		return 0;	//Invalid parameters
	tracknum = sp->track[track]->tracknum;
	tp = sp->pro_guitar_track[tracknum];
	if(notenum >= tp->notes)
		return 0;	//Invalid parameter

	flags = eof_get_note_flags(sp, track, notenum);
	if(ptr)
	{	//If the calling function passed a techniques structure
		ptr->length = eof_get_note_length(sp, track, notenum);
		fret = tp->note[notenum]->frets[stringnum] & 0x7F;	//Get the fret value for this string (mask out the muting bit)
		ptr->bendstrength_h = ptr->bendstrength_q = ptr->bend = 0;	//Initialize these to default values
		ptr->slideto = ptr->unpitchedslideto = -1;

		if((ptr->length == 1) && !(flags & EOF_PRO_GUITAR_NOTE_FLAG_BEND) && !(flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP) && !(flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN))
		{	//If the note is has the absolute minimum length and isn't a bend or a slide note (bend and slide notes are required to have a length > 0 or Rocksmith will crash)
			ptr->length = 0;	//Convert to a length of 0 so that it doesn't display as a sustain note in-game
		}
		if((flags & EOF_PRO_GUITAR_NOTE_FLAG_RS_NOTATION) == 0)
		{	//If this note doesn't have definitions for bend strength or the ending fret for slides
			if(flags & EOF_PRO_GUITAR_NOTE_FLAG_BEND)
			{	//If this note bends
				ptr->bend = 1;
				ptr->bendstrength_h = 1;	//Assume a 1 half step bend
				ptr->bendstrength_q = 2;
			}
			if(flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP)
			{	//If this note slides up and the user hasn't defined the ending fret of the slide
				ptr->slideto = fret + 1;	//Assume a 1 fret slide until logic is added for the author to add this information
			}
			else if(flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN)
			{	//If this note slides down and the user hasn't defined the ending fret of the slide
				ptr->slideto = fret - 1;	//Assume a 1 fret slide until logic is added for the author to add this information
			}
			ptr->slideto += tp->capo;	//Apply the capo position, so the slide ending is on the correct fret in-game
		}
		else
		{	//This note defines the bend strength and ending fret for slides
			if(flags & EOF_PRO_GUITAR_NOTE_FLAG_BEND)
			{	//If this note bends
				if(tp->note[notenum]->bendstrength & 0x80)
				{	//If this bend strength is defined in quarter steps
					ptr->bendstrength_q = (tp->note[notenum]->bendstrength & 0x7F);	//Obtain the defined bend strength in quarter steps (mask out the MSB)
					ptr->bendstrength_h = (ptr->bendstrength_q + 1) / 2;			//Obtain the defined bend strength rounded up to the nearest half step
				}
				else
				{	//The bend strength is defined in half steps
					ptr->bendstrength_h = tp->note[notenum]->bendstrength;	//Obtain the defined bend strength in half steps
					ptr->bendstrength_q = ptr->bendstrength_h * 2;			//Obtain the defined bend strength in quarter steps
				}
				ptr->bend = ptr->bendstrength_h;	//Obtain the strength of this bend rounded up to the nearest half step
			}
			if((flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP) || (flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN))
			{	//If this note slides
				ptr->slideto = tp->note[notenum]->slideend;
				if(eof_get_lowest_fretted_string_fret(sp, track, notenum) == ptr->slideto)
				{	//If the lowest fretted string in the note/chord slides to the position it's already at
					ptr->slideto = -1;	//Disable the slide
				}
				else
				{
					ptr->slideto += tp->capo;	//Apply the capo position, so the slide ending is on the correct fret in-game
				}
			}
		}
		if((flags & EOF_PRO_GUITAR_NOTE_FLAG_UNPITCH_SLIDE) && tp->note[notenum]->unpitchend)
		{	//If this note has an unpitched slide and the user has defined the ending fret of the slide
			if(eof_get_lowest_fretted_string_fret(sp, track, notenum) != tp->note[notenum]->unpitchend)
			{	//Don't allow the unpitched slide if it slides to the same fret this note/chord is already at
				ptr->unpitchedslideto = tp->note[notenum]->unpitchend;
				ptr->unpitchedslideto += tp->capo;	//Apply the capo position, so the slide ending is on the correct fret in-game
			}
		}

		//Determine note statuses
		ptr->hammeron = (flags & EOF_PRO_GUITAR_NOTE_FLAG_HO) ? 1 : 0;
		ptr->pulloff = (flags & EOF_PRO_GUITAR_NOTE_FLAG_PO) ? 1 : 0;
		ptr->harmonic = (flags & EOF_PRO_GUITAR_NOTE_FLAG_HARMONIC) ? 1 : 0;
		ptr->hopo = (ptr->hammeron | ptr->pulloff) ? 1 : 0;
		ptr->palmmute = (flags & EOF_PRO_GUITAR_NOTE_FLAG_PALM_MUTE) ? 1 : 0;
		ptr->tremolo = (flags & EOF_NOTE_FLAG_IS_TREMOLO) ? 1 : 0;
		ptr->pop = (flags & EOF_PRO_GUITAR_NOTE_FLAG_POP) ? 1 : -1;
		ptr->slap = (flags & EOF_PRO_GUITAR_NOTE_FLAG_SLAP) ? 1 : -1;
		ptr->accent = (flags & EOF_PRO_GUITAR_NOTE_FLAG_ACCENT) ? 1 : 0;
		ptr->pinchharmonic = (flags & EOF_PRO_GUITAR_NOTE_FLAG_P_HARMONIC) ? 1 : 0;
		ptr->stringmute = (flags & EOF_PRO_GUITAR_NOTE_FLAG_STRING_MUTE) ? 1 : 0;
		ptr->tap = (flags & EOF_PRO_GUITAR_NOTE_FLAG_TAP) ? 1 : 0;
		ptr->vibrato = (flags & EOF_PRO_GUITAR_NOTE_FLAG_VIBRATO) ? 80 : 0;
		ptr->linknext = (flags & EOF_PRO_GUITAR_NOTE_FLAG_LINKNEXT) ? 1 : 0;
		if((ptr->pop > 0) || (ptr->slap > 0))
		{	//If the note has pop or slap notation
			if(target == 1)
			{	//In Rocksmith 1, a slap/pop note cannot also be a slide/bend, because slap/pop notes require a sustain of 0 and slide/bend requires a nonzero sustain
				ptr->slideto = -1;	//Avoid allowing a 0 length slide note from crashing the game
				ptr->bend = ptr->bendstrength_h = ptr->bendstrength_q = 0;	//Avoid allowing a 0 length bend note from crashing the game
				ptr->length = 0;	//Remove all sustain for the note, otherwise Rocksmith 1 won't display the pop/slap sustain technique
			}
		}
	}//If the calling function passed a techniques structure

	//Make a bitmask reflecting only the techniques this note has that require a chordNote subtag to be written
	flags &= (	EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP | EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN | EOF_PRO_GUITAR_NOTE_FLAG_BEND | EOF_PRO_GUITAR_NOTE_FLAG_HO | EOF_PRO_GUITAR_NOTE_FLAG_PO |
				EOF_PRO_GUITAR_NOTE_FLAG_HARMONIC | EOF_NOTE_FLAG_IS_TREMOLO | EOF_PRO_GUITAR_NOTE_FLAG_POP | EOF_PRO_GUITAR_NOTE_FLAG_SLAP | EOF_PRO_GUITAR_NOTE_FLAG_P_HARMONIC |
				EOF_PRO_GUITAR_NOTE_FLAG_TAP | EOF_PRO_GUITAR_NOTE_FLAG_VIBRATO | EOF_PRO_GUITAR_NOTE_FLAG_LINKNEXT | EOF_PRO_GUITAR_NOTE_FLAG_UNPITCH_SLIDE);

	return flags;
}
