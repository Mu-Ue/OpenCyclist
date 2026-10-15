#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

// Posted to the window whenever the Strava state changes.
constexpr UINT WM_STRAVA = WM_APP + 16;

// Uploads rides to Strava. Signs in through the browser the first time and keeps the sign-in, encrypted for this
// Windows user, for next time. The work runs in the background; the state can be read at any time.
class Strava {
public:
    enum class State { SignedOut, SigningIn, Ready, Uploading, Uploaded, Failed };
    struct Status {
        State state = State::SignedOut;
        std::wstring athlete;  // the signed-in athlete's name
        std::wstring error;    // why it failed
        uint64_t activity = 0; // the uploaded activity
        bool signedIn = false;
    };

    explicit Strava(HWND window);
    ~Strava();
    Strava(const Strava&) = delete;
    Strava& operator=(const Strava&) = delete;

    // Whether this build was made with a Strava API app to sign in with.
    static bool Available();

    Status status() const;
    // Uploads a ride log, signing in first if needed. While signing in, starts the sign-in over.
    void Upload(std::wstring csvPath);
    // Forgets how the last upload went, for the next ride. Leaves an upload under way alone.
    void Clear();
    void SignOut();
    // Shows the uploaded activity in the browser.
    void OpenActivity() const;

    struct Shared;  // with the background work, which can outlive this

private:
    std::shared_ptr<Shared> shared_;
};
