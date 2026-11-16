// KartFrame crash reports: the question at the start after a run that crashed. The launch wrapper
// (frame/kartframe.sh) has the report prepared by frame/tools/crash-report.py and names its folder in
// KARTFRAME_CRASH_DIR. This panel shows it and writes the player's answer into that folder ("send" or
// "dont"); the wrapper acts on it. Nothing is sent by the game, and nothing unless the player chooses
// Send Report.
struct CrashPrompt {
    bool loaded=false,pending=false,sendable=false,showDetails=false;
    std::filesystem::path dir;
    std::string summary,details,error;
};
CrashPrompt g_crashPrompt;

std::string CrashPromptRead(const std::filesystem::path& path,size_t limit) {
    std::ifstream file(path,std::ios::binary);
    std::string text(limit,'\0');
    file.read(text.data(),static_cast<std::streamsize>(limit));
    text.resize(file ? limit : static_cast<size_t>(std::max<std::streamsize>(file.gcount(),0)));
    return text;
}

bool CrashPromptPending() {
    CrashPrompt& prompt=g_crashPrompt;
    if(!prompt.loaded) {
        prompt.loaded=true;
        const char* dir=std::getenv("KARTFRAME_CRASH_DIR");
        std::error_code ec;
        if(dir && *dir && std::filesystem::is_regular_file(std::filesystem::path(dir)/"whats-sent.txt",ec) &&
           !std::filesystem::exists(std::filesystem::path(dir)/"answer",ec)) {
            prompt.dir=dir;
            prompt.pending=true;
            prompt.sendable=std::filesystem::exists(prompt.dir/"sendable",ec);
            prompt.summary=CrashPromptRead(prompt.dir/"summary.txt",1024);
            prompt.details=CrashPromptRead(prompt.dir/"whats-sent.txt",1024*1024);
            RT_LOG(RT_TAG_RUNTIME) << "crash report from an earlier run: asking whether to send it" << std::endl;
        }
    }
    return prompt.pending;
}

void CrashPromptAnswer(const char* answer) {
    CrashPrompt& prompt=g_crashPrompt;
    const std::filesystem::path temporary=prompt.dir/"answer.tmp";
    {
        std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
        file<<answer<<'\n';
        if(!file) { prompt.error="Could not save your answer in "+RuntimeConfigFile::PathToUtf8(prompt.dir)+".";return; }
    }
    std::error_code ec;
    std::filesystem::rename(temporary,prompt.dir/"answer",ec);
    if(ec) { prompt.error="Could not save your answer: "+ec.message();return; }
    RT_LOG(RT_TAG_RUNTIME) << "crash report: the player chose " << answer << std::endl;
    prompt.pending=false;prompt.details.clear();
}

void DrawCrashPrompt() {
    CrashPrompt& prompt=g_crashPrompt;
    ImGui::TextUnformatted("KartFrame closed unexpectedly last time");ImGui::Separator();
    if(!prompt.summary.empty()) ImGui::TextWrapped("%s",prompt.summary.c_str());
    if(prompt.sendable) {
        ImGui::TextWrapped("You can send me this crash report so I can fix it. It goes to Sentry, a crash reporting service. "
            "Nothing is sent unless you choose Send Report, and then only this one report.");
    } else {
        ImGui::TextWrapped("This build can't send crash reports. To report it, run collect-logs.sh from the KartFrame tools "
            "folder and attach the archive to a GitHub issue.");
    }
    ImGui::TextWrapped("The report holds the reason, the stack, KartFrame's version, your graphics chip and driver, the VR "
        "settings and the last part of the game's log. Your user name, home folder, computer name and network addresses are "
        "removed. See What's Sent shows all of it.");
    const ImGuiStyle& style=ImGui::GetStyle();
    const int buttons=prompt.sendable?3:2;
    const float width=(ImGui::GetContentRegionAvail().x-style.ItemSpacing.x*(buttons-1))/buttons;
    if(ImGui::Button("Don't Send",ImVec2(width,64))) CrashPromptAnswer("dont");
    ImGui::SameLine();
    if(ImGui::Button(prompt.showDetails?"Hide What's Sent":"See What's Sent",ImVec2(width,64))) prompt.showDetails=!prompt.showDetails;
    if(prompt.sendable) {
        ImGui::SameLine();
        if(ImGui::Button("Send Report",ImVec2(width,64))) CrashPromptAnswer("send");
    }
    if(!prompt.error.empty()) ImGui::TextWrapped("%s",prompt.error.c_str());
    if(prompt.showDetails && prompt.pending) {
        ImGui::Separator();
        ImGui::BeginChild("CrashReportDetails");
        ImGui::TextUnformatted(prompt.details.data(),prompt.details.data()+prompt.details.size());
        ImGui::EndChild();
    }
}
