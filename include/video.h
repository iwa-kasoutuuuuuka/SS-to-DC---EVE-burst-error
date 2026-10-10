#ifndef VIDEO_H
#define VIDEO_H

/**
 * Play an MPEG-1 video file (blocking). A or Start cancels.
 * Returns 0 on success, -1 if the file is missing or cannot be played.
 */
int video_play(const char *filename);

#endif /* VIDEO_H */