#include "game/client_lua.h"

#include <atomic>
#include <cstdint>

#include "core/util.h"

namespace game::lua {

proto::ClientDownloadDataScNotify push(std::string code) {
    static std::atomic<uint32_t> version{static_cast<uint32_t>(util::nowMs() / 1000)};
    proto::ClientDownloadDataScNotify notify;
    auto& download = notify.download_data.emplace();
    download.version = ++version;
    download.time = static_cast<int64_t>(util::nowMs() / 1000);
    download.data = std::move(code);
    return notify;
}

// The client runs Luau, whose typeof is not xLua's, so the Canvas type comes from
// System.Type. Canvases keep their scripts running while switched off; UIRoot itself is
// only switched off when no canvas could be found.
std::string hideUi() {
    return R"(pcall(function()
    CapySRUi = CapySRUi or {}
    local canvasType = CS.System.Type.GetType("UnityEngine.Canvas, UnityEngine.UIModule")
    if canvasType ~= nil then
        local canvases = CS.UnityEngine.Object.FindObjectsOfType(canvasType)
        for i = 0, canvases.Length - 1 do
            local canvas = canvases[i]
            if canvas.enabled and canvas.isRootCanvas then
                canvas.enabled = false
                table.insert(CapySRUi, canvas)
            end
        end
    end
    if #CapySRUi == 0 then
        local root = CS.UnityEngine.GameObject.Find("UIRoot")
        if root ~= nil then
            root:SetActive(false)
            CapySRUiRoot = root
        end
    end
end)
)";
}

std::string showUi() {
    return R"(pcall(function()
    for _, canvas in ipairs(CapySRUi or {}) do
        pcall(function() canvas.enabled = true end)
    end
    CapySRUi = {}
    if CapySRUiRoot ~= nil then
        pcall(function() CapySRUiRoot:SetActive(true) end)
        CapySRUiRoot = nil
    end
end)
)";
}

}  // namespace game::lua
