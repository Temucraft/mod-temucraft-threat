-- PetTank: turn the server's pet tank bonuses on or off for yourself, per place, where the server allows them.
--
-- Talks to the module with addon whispers to yourself (prefix PETTANK, see src/PetTankCommands.cpp). A server
-- without the module never answers: after a few tries the addon hides (and still listens, so a late answer brings
-- it back; /pettank asks again).
--
-- /pettank   show or hide the window

local ADDON_VERSION = 1   -- must match PET_TANK_ADDON_VERSION in the module (src/PetTank.h)
local PREFIX = "PETTANK"
local ASK_TRIES = 4

local available = nil     -- nil waiting, true the server answered, false no answer yet
local serverVersion
local replyTimeout
local askTries = 0
local autoRefresh = 0
local minimapButton, optionsPanel

-- The places, in the module's order (PetTankPlace)
local PLACES = {
    { key = "world",   name = "Open world" },
    { key = "normal",  name = "Dungeons (normal)" },
    { key = "heroic",  name = "Dungeons (heroic)" },
    { key = "raid10",  name = "Raids, 10 player" },
    { key = "raid10h", name = "Raids, 10 player heroic" },
    { key = "raid25",  name = "Raids, 25 player" },
    { key = "raid25h", name = "Raids, 25 player heroic" },
    { key = "bg",      name = "Battlegrounds" },
    { key = "arena",   name = "Arenas" },
}
local GROUP_NAMES = { [1] = "solo", [2] = "in a party", [4] = "in a raid group" }

-- From the server's "S"
local state = { on = false, toggle = false, place = 0, group = 1, active = false, off = 0, where = {}, hasPet = false,
    pets = 0, happyMode = 0, happyAuto = false, happyMin = 0, petHappy = -1 }

local function Send(body)
    SendAddonMessage(PREFIX, body, "WHISPER", UnitName("player"))
end

local function Print(text)
    DEFAULT_CHAT_FRAME:AddMessage("|cff8FD14F[Pet Tank]|r " .. text)
end

local function Split(text)
    local out = {}
    for piece in string.gmatch(text .. "\t", "(.-)\t") do table.insert(out, piece) end
    return out
end

local function Bit(mask, i) return math.floor(mask / 2 ^ i) % 2 == 1 end

local function GroupsText(bits)
    if bits == 0 then return "|cffFF6060off on this server|r" end
    if bits == 7 then return "|cff999999solo, party and raid|r" end
    local list = {}
    if bits % 2 == 1 then table.insert(list, "solo") end
    if math.floor(bits / 2) % 2 == 1 then table.insert(list, "party") end
    if math.floor(bits / 4) % 2 == 1 then table.insert(list, "raid") end
    return "|cffFFD100" .. table.concat(list, ", ") .. " only|r"
end

-- ---------------------------------------------------------------- the window

local frame = CreateFrame("Frame", "PetTankFrame", UIParent)
frame:SetWidth(420); frame:SetHeight(440)
frame:SetPoint("CENTER")
frame:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
frame:SetMovable(true)
frame:EnableMouse(true)
frame:RegisterForDrag("LeftButton")
frame:SetScript("OnDragStart", frame.StartMoving)
frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
frame:SetFrameStrata("DIALOG")
frame:Hide()
table.insert(UISpecialFrames, "PetTankFrame")

local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", -6, -6)

local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
title:SetPoint("TOP", 0, -18)
title:SetText("Pet Tank")

local status = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
status:SetPoint("TOPLEFT", 22, -44)
status:SetWidth(376)
status:SetJustifyH("LEFT")

local help = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
help:SetPoint("TOPLEFT", status, "BOTTOMLEFT", 0, -6)
help:SetWidth(376)
help:SetJustifyH("LEFT")

local rows = {}
for i, place in ipairs(PLACES) do
    local check = CreateFrame("CheckButton", "PetTankCheck" .. i, frame, "UICheckButtonTemplate")
    check:SetWidth(24); check:SetHeight(24)
    check:SetPoint("TOPLEFT", 20, -96 - (i - 1) * 24)
    local label = _G["PetTankCheck" .. i .. "Text"]
    label:SetText(place.name)
    label:SetFontObject("GameFontHighlight")
    local allowed = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    allowed:SetPoint("LEFT", check, "LEFT", 210, 0)
    allowed:SetJustifyH("LEFT")
    local here = frame:CreateTexture(nil, "BACKGROUND")
    here:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
    here:SetBlendMode("ADD")
    here:SetPoint("TOPLEFT", check, "TOPLEFT", -4, 0)
    here:SetWidth(384); here:SetHeight(24)
    here:Hide()
    check:SetScript("OnClick", function(self)
        local off = state.off
        local bitValue = 2 ^ (i - 1)
        if self:GetChecked() then
            if Bit(off, i - 1) then off = off - bitValue end
        elseif not Bit(off, i - 1) then
            off = off + bitValue
        end
        Send("SET\t" .. off)
    end)
    rows[i] = { check = check, label = label, allowed = allowed, here = here }
end

-- Hunter pet happiness (PetTank.Happiness.Minimum): where it stands, and raising it by itself or by hand
local happyTitle = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
happyTitle:SetPoint("TOPLEFT", 22, -96 - #PLACES * 24 - 10)
happyTitle:SetText("Hunter pet happiness")
local happyText = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
happyText:SetPoint("TOPLEFT", happyTitle, "BOTTOMLEFT", 0, -4)
happyText:SetWidth(376)
happyText:SetJustifyH("LEFT")
local happyAuto = CreateFrame("CheckButton", "PetTankHappyAuto", frame, "UICheckButtonTemplate")
happyAuto:SetWidth(24); happyAuto:SetHeight(24)
happyAuto:SetPoint("TOPLEFT", happyText, "BOTTOMLEFT", -4, -4)
PetTankHappyAutoText:SetText("Raise it by itself")
PetTankHappyAutoText:SetFontObject("GameFontHighlight")
happyAuto:SetScript("OnClick", function(self) Send("HAPPYAUTO\t" .. (self:GetChecked() and "1" or "0")) end)
local happyButton = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
happyButton:SetWidth(120); happyButton:SetHeight(22)
happyButton:SetPoint("LEFT", happyAuto, "RIGHT", 150, 0)
happyButton:SetText("Make it happy")
happyButton:SetScript("OnClick", function() Send("HAPPY") end)

local function HappyName(percent)
    if percent < 32 then return "|cffFF6060unhappy|r, 75% damage" end
    if percent < 63 then return "|cffFFD100content|r, 100% damage" end
    return "|cff4CFF00happy|r, 125% damage"
end

local note = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
note:SetPoint("BOTTOMLEFT", 22, 20)
note:SetWidth(376)
note:SetJustifyH("LEFT")

local function Redraw()
    if not frame:IsShown() then return end
    if not state.on then
        status:SetText("|cffFF6060The pet tank bonuses are off on this server.|r")
    else
        local here = PLACES[state.place + 1]
        status:SetText(string.format("Here: %s, %s. Bonuses %s.", here and here.name or "?",
            GROUP_NAMES[state.group] or "?", state.active and "|cff4CFF00on|r" or "|cffFF6060off|r"))
    end
    help:SetText((state.pets == 1 and "Every pet counts." or "Pets with a taunt count (Growl, Voidwalker, " ..
        "Felguard, Greater Earth Elemental).") .. (state.hasPet and "" or " |cffFF6060You have no pet that " ..
        "counts out right now.|r") .. "\nMore threat, faster regeneration out of combat, and Mend Pet / Health " ..
        "Funnel heal more for less.")
    for i, row in ipairs(rows) do
        local bits = tonumber(state.where[i]) or 0
        row.allowed:SetText(GroupsText(bits))
        row.check:SetChecked(not Bit(state.off, i - 1))
        if bits == 0 or not state.toggle or not state.on then row.check:Disable() else row.check:Enable() end
        if state.place == i - 1 then row.here:Show() else row.here:Hide() end
    end
    local pet = state.petHappy >= 0 and string.format("Your pet: %d%% (%s).", state.petHappy,
        HappyName(state.petHappy)) or "No hunter pet out."
    if state.happyMode == 0 then
        happyText:SetText(pet .. " No minimum on this server: feed your pet to keep it happy.")
        happyAuto:Hide(); happyButton:Hide()
    elseif state.happyMode == 1 then
        happyText:SetText(pet .. string.format(" The server keeps it at %d%% at least, by itself.", state.happyMin))
        happyAuto:Hide(); happyButton:Hide()
    else
        happyText:SetText(pet .. string.format(" Minimum %d%%: by itself, or with the button.", state.happyMin))
        happyAuto:Show(); happyButton:Show()
        happyAuto:SetChecked(state.happyAuto)
        local low = state.petHappy >= 0 and state.petHappy < state.happyMin
        if low then happyButton:Enable() else happyButton:Disable() end
    end
    note:SetText(state.toggle and "Ticked: on for you where the server allows it. Untick to turn it off for you " ..
        "there." or "This server decides alone where the bonuses are on.")
end

-- ---------------------------------------------------------------- talking to the server

local events = CreateFrame("Frame")

local function Ask()
    askTries = askTries + 1
    replyTimeout = 10
    Send("HELLO\t" .. ADDON_VERSION)
end

local function UpdateMinimapButton() end -- set below

local function OnMessage(message)
    local parts = Split(message)
    if available ~= true then
        local wasHidden = available == false
        available = true
        replyTimeout = nil
        if wasHidden then UpdateMinimapButton() end
    end
    local kind = parts[1]
    if kind == "V" then
        serverVersion = tonumber(parts[2])
        if serverVersion and serverVersion ~= ADDON_VERSION then
            Print(string.format("This addon is version %d and the server expects %d: update it.", ADDON_VERSION,
                serverVersion))
        end
    elseif kind == "S" then
        state.on = parts[2] == "1"
        state.toggle = parts[3] == "1"
        state.place = tonumber(parts[4]) or 0
        state.group = tonumber(parts[5]) or 1
        state.active = parts[6] == "1"
        state.off = tonumber(parts[7]) or 0
        state.where = {}
        for value in string.gmatch(parts[8] or "", "[^,]+") do table.insert(state.where, value) end
        state.hasPet = parts[9] == "1"
        state.pets = tonumber(parts[10]) or 0
        state.happyMode = tonumber(parts[11]) or 0
        state.happyAuto = parts[12] == "1"
        state.happyMin = tonumber(parts[13]) or 0
        state.petHappy = tonumber(parts[14]) or -1
        Redraw()
    elseif kind == "X" then
        Print(parts[2] or "")
    end
end

local loginDelay
events:RegisterEvent("PLAYER_LOGIN")
events:RegisterEvent("PLAYER_ENTERING_WORLD")
events:RegisterEvent("CHAT_MSG_ADDON")
events:RegisterEvent("PARTY_MEMBERS_CHANGED")
events:RegisterEvent("RAID_ROSTER_UPDATE")
events:SetScript("OnEvent", function(self, event, prefix, message, channel, sender)
    if event == "CHAT_MSG_ADDON" then
        if prefix == PREFIX and channel == "WHISPER" and sender == UnitName("player") then OnMessage(message) end
    elseif event == "PLAYER_LOGIN" then
        loginDelay = 3
    elseif event == "PLAYER_ENTERING_WORLD" then
        if available ~= true and not loginDelay and not replyTimeout then
            askTries = 0
            Ask()
        elseif available and frame:IsShown() then
            Send("GET")
        end
    elseif available and frame:IsShown() then
        Send("GET")
    end
end)

events:SetScript("OnUpdate", function(self, elapsed)
    if frame:IsShown() and available then
        autoRefresh = autoRefresh - elapsed
        if autoRefresh <= 0 then
            autoRefresh = 5
            Send("GET")
        end
    end
    if loginDelay then
        loginDelay = loginDelay - elapsed
        if loginDelay <= 0 then
            loginDelay = nil
            askTries = 0
            Ask()
        end
    elseif replyTimeout and not available then
        replyTimeout = replyTimeout - elapsed
        if replyTimeout <= 0 then
            if askTries < ASK_TRIES then
                Ask()
            else
                replyTimeout = nil
                available = false
                frame:Hide()
                UpdateMinimapButton()
            end
        end
    end
end)

frame:SetScript("OnShow", function()
    autoRefresh = 5
    Send("GET")
    Redraw()
end)

SLASH_PETTANK1 = "/pettank"
SlashCmdList["PETTANK"] = function()
    if available == false then
        Print("Asking the server again...")
        available = nil
        askTries = ASK_TRIES - 1
        Ask()
        return
    elseif available == nil then
        Print("Still waiting for the server to answer, try again in a few seconds.")
        return
    end
    if frame:IsShown() then frame:Hide() else frame:Show() end
end

-- ---------------------------------------------------------------- minimap button and Interface > AddOns panel

local UI_NAME = "PetTank"
local UI_TITLE = "Pet Tank"
local UI_ICON = "Interface\\Icons\\Ability_Physical_Taunt"
local UI_DEFAULT_ANGLE = 245
local function UI_DB()
    PetTankDB = PetTankDB or {}
    return PetTankDB
end
local function UI_STATUS()
    if available then
        local text = "|cff4CFF00Connected.|r Bonuses here: " .. (state.active and "on" or "off") .. "."
        if serverVersion and serverVersion ~= ADDON_VERSION then
            text = text .. " |cffFF6060Addon version " .. ADDON_VERSION .. ", the server expects " .. serverVersion ..
                ".|r"
        end
        return text
    end
    if available == false then
        return "|cffFF6060No answer:|r this server doesn't seem to have pet tanks. /pettank asks again."
    end
    return "Waiting for the server to answer..."
end

local function PlaceMinimapButton()
    local angle = math.rad(UI_DB().minimapAngle or UI_DEFAULT_ANGLE)
    minimapButton:ClearAllPoints()
    minimapButton:SetPoint("CENTER", Minimap, "CENTER", math.cos(angle) * 80, math.sin(angle) * 80)
end

-- Hidden until the server answers, and on servers without the module
UpdateMinimapButton = function()
    if not minimapButton then return end
    if available ~= true or UI_DB().hideMinimap then minimapButton:Hide() else minimapButton:Show() end
end

local function CreateMinimapButton()
    minimapButton = CreateFrame("Button", UI_NAME .. "MinimapButton", Minimap)
    minimapButton:SetWidth(31); minimapButton:SetHeight(31)
    minimapButton:SetFrameStrata("MEDIUM")
    minimapButton:SetFrameLevel(8)
    minimapButton:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    minimapButton:RegisterForDrag("LeftButton")
    minimapButton:SetHighlightTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight")
    local background = minimapButton:CreateTexture(nil, "BACKGROUND")
    background:SetWidth(20); background:SetHeight(20)
    background:SetTexture("Interface\\Minimap\\UI-Minimap-Background")
    background:SetPoint("TOPLEFT", 7, -5)
    local icon = minimapButton:CreateTexture(nil, "ARTWORK")
    icon:SetWidth(17); icon:SetHeight(17)
    icon:SetTexture(UI_ICON)
    icon:SetPoint("TOPLEFT", 7, -6)
    local border = minimapButton:CreateTexture(nil, "OVERLAY")
    border:SetWidth(53); border:SetHeight(53)
    border:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
    border:SetPoint("TOPLEFT")
    minimapButton:SetScript("OnDragStart", function(self)
        self:SetScript("OnUpdate", function()
            local mx, my = Minimap:GetCenter()
            local x, y = GetCursorPosition()
            local scale = Minimap:GetEffectiveScale()
            UI_DB().minimapAngle = math.deg(math.atan2(y / scale - my, x / scale - mx))
            PlaceMinimapButton()
        end)
    end)
    minimapButton:SetScript("OnDragStop", function(self) self:SetScript("OnUpdate", nil) end)
    minimapButton:SetScript("OnClick", function(self, button)
        if button == "RightButton" then
            InterfaceOptionsFrame_OpenToCategory(optionsPanel)
        else
            SlashCmdList["PETTANK"]()
        end
    end)
    minimapButton:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_LEFT")
        GameTooltip:AddLine(UI_TITLE)
        GameTooltip:AddLine(UI_STATUS(), 1, 1, 1, true)
        GameTooltip:AddLine("Left-click: where your pet's tank bonuses are on", 0.6, 0.8, 1)
        GameTooltip:AddLine("Right-click: options. Drag: move this button.", 0.6, 0.8, 1)
        GameTooltip:Show()
    end)
    minimapButton:SetScript("OnLeave", function() GameTooltip:Hide() end)
    PlaceMinimapButton()
    UpdateMinimapButton()
end

optionsPanel = CreateFrame("Frame", UI_NAME .. "Options", UIParent)
optionsPanel.name = UI_TITLE
optionsPanel:Hide()
local optionsTitle = optionsPanel:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
optionsTitle:SetPoint("TOPLEFT", 16, -16)
optionsTitle:SetText(UI_TITLE)
local optionsText = optionsPanel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
optionsText:SetPoint("TOPLEFT", optionsTitle, "BOTTOMLEFT", 0, -8)
optionsText:SetWidth(560)
optionsText:SetJustifyH("LEFT")
optionsText:SetText("Where your pet's tank bonuses are on for you: more threat, faster " ..
    "regeneration out of combat, and Mend Pet / Health Funnel heal more for less. Only on servers with the " ..
    "module.\n\n/pettank - show or hide the window\n.pettank - the same in chat")
local optionsStatus = optionsPanel:CreateFontString(nil, "ARTWORK", "GameFontNormal")
optionsStatus:SetPoint("TOPLEFT", optionsText, "BOTTOMLEFT", 0, -12)
optionsStatus:SetWidth(560)
optionsStatus:SetJustifyH("LEFT")
local minimapCheck = CreateFrame("CheckButton", UI_NAME .. "MinimapCheck", optionsPanel,
    "InterfaceOptionsCheckButtonTemplate")
minimapCheck:SetPoint("TOPLEFT", optionsStatus, "BOTTOMLEFT", -2, -12)
_G[UI_NAME .. "MinimapCheckText"]:SetText("Show the minimap button")
minimapCheck:SetScript("OnClick", function(self)
    UI_DB().hideMinimap = not self:GetChecked()
    UpdateMinimapButton()
end)
optionsPanel:SetScript("OnShow", function()
    optionsStatus:SetText(UI_STATUS())
    minimapCheck:SetChecked(not UI_DB().hideMinimap)
end)
InterfaceOptions_AddCategory(optionsPanel)

local uiEvents = CreateFrame("Frame")
uiEvents:RegisterEvent("PLAYER_LOGIN")
uiEvents:SetScript("OnEvent", CreateMinimapButton)
