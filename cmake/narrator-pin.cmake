# SDD-013: the narrator model this build is for. The one place it is named:
# ModelFile.cpp is compiled with these three (cmake/NarratorModel.cmake passes
# them in), and the bundle a package carries is downloaded and checked against
# the same three. The runtime's prompt contract (include/model/LocalModel.h) is
# this file's; a new model moves all of them together.
#
# narrator-v1: the release asset of hms-homelab/hms-cpap tag narrator-v1,
# 47,175,872 bytes, Q4_K_M, exported with llama.cpp 919794500475862f.
set(HMS_NARRATOR_VERSION "narrator-v1")
set(HMS_NARRATOR_URL
    "https://github.com/hms-homelab/hms-cpap/releases/download/narrator-v1/narrator-v1-q4_k_m.gguf")
set(HMS_NARRATOR_SHA256 "a0d99c56137466735fa0371dd64e6bdb908b2e33cd250632670534734af48d00")
