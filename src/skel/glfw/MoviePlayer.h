#ifndef MOVIEPLAYER_H
#define MOVIEPLAYER_H

namespace MoviePlayer
{
void Play(const char *path);

void Draw();

bool IsActive();

void SetPaused(bool paused);

void Stop();
}

#endif
