# Profiles

A profile is everything PortableKit needs for one title, kept out of the reusable framework: the release it supports, its memory layout and overlay slots, its save folders, and the code that knows the game's own addresses.

The current repository includes the `mhp3rd` profile, which the top-level CMakeLists builds. See PortableKit's [`docs/PROFILE_GUIDE.md`](../portablekit/docs/PROFILE_GUIDE.md) before adding another title; a new game is started in its own repository, with PortableKit as a submodule.
