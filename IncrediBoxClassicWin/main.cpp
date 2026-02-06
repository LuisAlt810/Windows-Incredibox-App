// main.cpp
// Incredibox-like C++ Windows app using SDL2 + SDL2_mixer + dr_wav for export
// Requirements: SDL2, SDL2_mixer, dr_wav.h (place next to this file)

#include <SDL.h>
#include <SDL_mixer.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <cstring>

// dr_wav single-header must be downloaded separately and placed in project folder.
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

namespace fs = std::filesystem;

const int WINDOW_W = 900;
const int WINDOW_H = 360;
const int NUM_SLOTS = 8;

struct Slot {
    std::string path;
    Mix_Chunk* chunk = nullptr;
    bool active = false;
    bool solo = false;
    int volume = MIX_MAX_VOLUME; // 0-128
};

std::vector<Slot> slots(NUM_SLOTS);
std::vector<int> channels(NUM_SLOTS, -1);

std::atomic<bool> run_loop { true };
std::atomic<int> BPM { 100 };
const int BEATS_PER_BAR = 4;
const int BARS = 4;
std::mutex audio_mtx;

double get_loop_seconds() {
    double beat_interval = 60.0 / static_cast<double>(BPM.load());
    return beat_interval * BEATS_PER_BAR * BARS;
}

void audio_loop_thread() {
    using namespace std::chrono;
    while (run_loop.load()) {
        double loop_sec = get_loop_seconds();
        auto loop_start = steady_clock::now();
        // Start all active slots at loop start
        {
            std::lock_guard<std::mutex> lock(audio_mtx);
            // check solo: if any slot.solo true, only play solos
            bool any_solo = false;
            for (auto &s : slots) if (s.solo) { any_solo = true; break; }
            for (int i = 0; i < NUM_SLOTS; ++i) {
                Slot &s = slots[i];
                if (s.chunk && s.active) {
                    if (any_solo && !s.solo) continue;
                    channels[i] = Mix_PlayChannel(i, s.chunk, -1); // loop forever, we'll stop later
                    Mix_Volume(i, s.volume);
                }
            }
        }
        // Sleep until loop end
        std::this_thread::sleep_for(duration<double>(loop_sec));
        // Stop channels to re-align next loop
        {
            std::lock_guard<std::mutex> lock(audio_mtx);
            for (int i = 0; i < NUM_SLOTS; ++i) {
                if (channels[i] >= 0) {
                    Mix_HaltChannel(channels[i]);
                    channels[i] = -1;
                }
            }
        }
        // tiny safety sleep (allows UI to respond)
        std::this_thread::sleep_for(milliseconds(5));
        // next loop iteration continues
    }
}

// mix WAV files (16-bit PCM) into a single WAV and write with dr_wav
bool export_mix_wav(const std::string &outPath, double duration_sec) {
    // We will read each active slot's wav using dr_wav, loop/trim to duration, mix as int32 accumulator.
    const int target_samplerate = 44100; // we'll enforce this (or resampling needed)
    std::vector<int16_t*> data_ptrs(NUM_SLOTS, nullptr);
    std::vector<uint64_t> frames(NUM_SLOTS, 0);
    std::vector<unsigned int> chans(NUM_SLOTS, 0);
    std::vector<unsigned int> sr(NUM_SLOTS, 0);

    uint64_t total_frames = static_cast<uint64_t>(duration_sec * target_samplerate);

    for (int i = 0; i < NUM_SLOTS; ++i) {
        if (!slots[i].active) continue;
        const char* path = slots[i].path.c_str();
        unsigned int channels_out = 0;
        unsigned int sr_out = 0;
        drwav_uint64 totalFrameCount = 0;
        int16_t* pcm = drwav_open_file_and_read_pcm_frames_s16(path, &channels_out, &sr_out, &totalFrameCount, nullptr);
        if (!pcm) {
            std::cerr << "Failed to load WAV for export: " << path << "\n";
            // cleanup
            for (int j=0;j<NUM_SLOTS;++j) if (data_ptrs[j]) drwav_free(data_ptrs[j], nullptr);
            return false;
        }
        data_ptrs[i] = pcm;
        frames[i] = totalFrameCount;
        chans[i] = channels_out;
        sr[i] = sr_out;
        if (sr_out != (unsigned)target_samplerate) {
            std::cerr << "Warning: sample rate mismatch for " << path << " (" << sr_out << " Hz). Export assumes 44100 Hz.\n";
        }
    }

    // Mix into int32 buffer (interleaved stereo)
    std::vector<int32_t> mixbuf(total_frames * 2, 0); // stereo output
    for (int i = 0; i < NUM_SLOTS; ++i) {
        if (!data_ptrs[i]) continue;
        unsigned int in_ch = chans[i];
        uint64_t in_frames = frames[i];
        int16_t* pcm = data_ptrs[i];
        // For simplicity: if input is mono, duplicate into stereo; if stereo use both channels.
        for (uint64_t f = 0; f < total_frames; ++f) {
            // take frame from input with wrap (looping)
            uint64_t src_frame = f % in_frames;
            int idx_in = static_cast<int>(src_frame * in_ch);
            int16_t inL = 0, inR = 0;
            if (in_ch == 1) {
                inL = pcm[idx_in];
                inR = pcm[idx_in];
            } else {
                inL = pcm[idx_in];
                inR = pcm[idx_in + 1];
            }
            // apply per-slot volume: slots[i].volume (0-128) -> scale
            float volf = slots[i].volume / 128.0f;
            mixbuf[f*2 + 0] += static_cast<int32_t>(inL * volf);
            mixbuf[f*2 + 1] += static_cast<int32_t>(inR * volf);
        }
    }

    // clamp to int16 and write out using dr_wav
    drwav_data_format format;
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = 2;
    format.sampleRate = target_samplerate;
    format.bitsPerSample = 16;

    drwav* pWav = drwav_open_file_write(outPath.c_str(), &format);
    if (!pWav) {
        std::cerr << "Failed to open output WAV for writing: " << outPath << "\n";
        for (int j=0;j<NUM_SLOTS;++j) if (data_ptrs[j]) drwav_free(data_ptrs[j], nullptr);
        return false;
    }

    // prepare buffer of int16
    std::vector<int16_t> outbuf(total_frames * 2);
    for (uint64_t i = 0; i < total_frames * 2; ++i) {
        int64_t v = mixbuf[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        outbuf[i] = static_cast<int16_t>(v);
    }

    drwav_uint64 frames_written = drwav_write_pcm_frames(pWav, total_frames, outbuf.data());
    drwav_close(pWav);

    // cleanup
    for (int j=0;j<NUM_SLOTS;++j) if (data_ptrs[j]) drwav_free(data_ptrs[j], nullptr);

    std::cout << "Export complete: " << outPath << " (" << frames_written << " frames)\n";
    return true;
}

int main(int argc, char** argv) {
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::cerr << "SDL_Init error: " << SDL_GetError() << "\n";
        return 1;
    }

    // Initialize SDL_mixer
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        std::cerr << "Mix_OpenAudio error: " << Mix_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    // Reserve channels
    Mix_AllocateChannels(NUM_SLOTS);

    // Load samples from samples/slotX.wav
    fs::path base = fs::current_path() / "samples";
    for (int i = 0; i < NUM_SLOTS; ++i) {
        fs::path p = base / ("slot" + std::to_string(i) + ".wav");
        slots[i].path = p.string();
        if (fs::exists(p)) {
            slots[i].chunk = Mix_LoadWAV(p.string().c_str());
            if (!slots[i].chunk) {
                std::cerr << "Failed to load sample " << p << " : " << Mix_GetError() << "\n";
            } else {
                std::cout << "Loaded sample " << p << "\n";
            }
        } else {
            std::cout << "Missing sample " << p << " (slot will be empty)\n";
        }
    }

    SDL_Window* window = SDL_CreateWindow("Incredibox Classic - Win",
                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            WINDOW_W, WINDOW_H, SDL_WINDOW_SHOWN);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        Mix_CloseAudio();
        SDL_Quit();
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    // Start audio loop thread
    std::thread audio_thread(audio_loop_thread);

    // UI state
    bool quit = false;
    SDL_Event e;
    // simple rectangles for slots
    int margin = 20;
    int w = (WINDOW_W - margin*2 - (NUM_SLOTS/2 -1)*10) / (NUM_SLOTS/2);
    int h = 120;
    int spacing = 10;

    auto draw_text_simple = [&](int x, int y, const char* t) {
        // Very simple placeholder: no text rendering library used.
        // We will not render text here; instead rely on console messages and simple colored boxes.
    };

    while (!quit) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { quit = true; break; }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x, my = e.button.y;
                // check which slot clicked
                for (int i = 0; i < NUM_SLOTS; ++i) {
                    int col = i % (NUM_SLOTS/2);
                    int row = i / (NUM_SLOTS/2);
                    int x = margin + col * (w + spacing);
                    int y = margin + row * (h + spacing);
                    SDL_Rect r = { x, y, w, h };
                    if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h) {
                        // toggle active
                        slots[i].active = !slots[i].active;
                        std::cout << "Slot " << i << (slots[i].active ? " ON\n" : " OFF\n");
                    }
                }
            }
            if (e.type == SDL_KEYDOWN) {
                // keys: Up/Down change BPM, E export, S toggle solo last slot, number keys toggle volume lower/higher for last selected slot
                if (e.key.keysym.sym == SDLK_UP) {
                    BPM += 5;
                    std::cout << "BPM: " << BPM.load() << "\n";
                } else if (e.key.keysym.sym == SDLK_DOWN) {
                    BPM = std::max(40, BPM.load() - 5);
                    std::cout << "BPM: " << BPM.load() << "\n";
                } else if (e.key.keysym.sym == SDLK_e) {
                    std::cout << "Exporting mix.wav for 8 seconds...\n";
                    export_mix_wav("exported_mix.wav", 8.0);
                } else if (e.key.keysym.sym == SDLK_q) {
                    quit = true;
                }
            }
        }

        // Render simple UI
        SDL_SetRenderDrawColor(renderer, 24, 24, 24, 255);
        SDL_RenderClear(renderer);

        // Draw slots
        for (int i = 0; i < NUM_SLOTS; ++i) {
            int col = i % (NUM_SLOTS/2);
            int row = i / (NUM_SLOTS/2);
            int x = margin + col * (w + spacing);
            int y = margin + row * (h + spacing);
            SDL_Rect r = { x, y, w, h };
            if (slots[i].active) {
                SDL_SetRenderDrawColor(renderer, 40, 180, 99, 255); // greenish if active
            } else {
                SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
            }
            SDL_RenderFillRect(renderer, &r);
            // border
            SDL_SetRenderDrawColor(renderer, 0,0,0,255);
            SDL_RenderDrawRect(renderer, &r);
        }

        // Draw bottom info bar (BPM + controls)
        SDL_Rect info = {0, WINDOW_H - 40, WINDOW_W, 40};
        SDL_SetRenderDrawColor(renderer, 12,12,12,255);
        SDL_RenderFillRect(renderer, &info);
        // Note: keeping text out of SDL rendering for simplicity; use console for text instructions
        SDL_RenderPresent(renderer);

        SDL_Delay(16); // ~60fps
    }

    // shutdown
    run_loop = false;
    if (audio_thread.joinable()) audio_thread.join();

    for (int i = 0; i < NUM_SLOTS; ++i) {
        if (slots[i].chunk) Mix_FreeChunk(slots[i].chunk);
    }
    Mix_CloseAudio();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::cout << "Exiting\n";
    return 0;
}
