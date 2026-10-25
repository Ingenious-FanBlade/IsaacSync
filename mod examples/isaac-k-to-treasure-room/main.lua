local KToTreasureRoom = RegisterMod("K to Treasure Room", 1)
local game = Game()

-- This manifest must be byte-for-byte identical on every peer.
local SYNC_ID = "dev.isaacsync.k-to-treasure-room"
local SYNC_VERSION = 1
local OPCODE_GO_TO_TREASURE = 1

local registered = false
local syncPort = nil
local startRequested = false
local initializationFailed = false
local activeSession = false
local lastTransitionFrame = nil

local function log(message)
    Isaac.DebugString("[K to Treasure Room] " .. tostring(message))
end

local function findTreasureRoomIndex()
    local level = game:GetLevel()
    local rooms = level:GetRooms()
    local selectedIndex = nil

    -- Pick the lowest grid index explicitly. This is deterministic even on
    -- custom floors that happen to contain more than one Treasure Room.
    for i = 0, rooms.Size - 1 do
        local room = rooms:Get(i)
        if room ~= nil and room.Data ~= nil and room.Data.Type == RoomType.ROOM_TREASURE then
            local roomIndex = room.SafeGridIndex
            if selectedIndex == nil or roomIndex < selectedIndex then
                selectedIndex = roomIndex
            end
        end
    end

    return selectedIndex
end

local function isTreasureRoomIndex(roomIndex)
    if type(roomIndex) ~= "number" or math.type(roomIndex) ~= "integer" or roomIndex < 0 then
        return false
    end

    local descriptor = game:GetLevel():GetRoomByIdx(roomIndex, -1)
    return descriptor ~= nil and descriptor.Data ~= nil and
        descriptor.Data.Type == RoomType.ROOM_TREASURE
end

local function moveAllPlayersToTreasureRoom(roomIndex)
    if not isTreasureRoomIndex(roomIndex) then
        log("Refused invalid or mismatched Treasure Room index " .. tostring(roomIndex))
        return
    end

    -- Leaving Player as nil performs one game-level transition, so every local
    -- co-op player moves together. Repeating this once per player would interrupt
    -- the preceding transition.
    game:StartRoomTransition(
        roomIndex,
        Direction.NO_DIRECTION,
        RoomTransitionAnim.TELEPORT,
        nil,
        -1
    )
end

local function onSyncOperation(senderSlot, opcode, payload, frame)
    local peerCount = Isaac.Sync.GetPeerCount()
    if type(peerCount) ~= "number" or math.type(peerCount) ~= "integer" or
       type(senderSlot) ~= "number" or math.type(senderSlot) ~= "integer" or
       senderSlot < 0 or senderSlot >= peerCount then
        log("Ignored operation with invalid sender slot " .. tostring(senderSlot))
        return
    end
    if opcode ~= OPCODE_GO_TO_TREASURE then
        log("Ignored unknown opcode " .. tostring(opcode))
        return
    end

    -- Both peers can press K in the same synchronized frame. The dispatcher
    -- invokes slot 0 then slot 1, so collapse identical-frame requests into
    -- one game transition on both peers rather than interrupting it twice.
    if lastTransitionFrame == frame then
        return
    end
    lastTransitionFrame = frame

    -- The sender supplies the chosen grid index. Each peer verifies that the
    -- same index is a Treasure Room before doing the game-level transition.
    moveAllPlayersToTreasureRoom(payload)
end

local function getSync()
    local sync = Isaac.Sync
    if type(sync) ~= "table" or type(sync.Register) ~= "function" or
       type(sync.Start) ~= "function" or type(sync.IsActive) ~= "function" or
       type(sync.GetPeerCount) ~= "function" or type(sync.Send) ~= "function" then
        return nil
    end

    if initializationFailed then
        return nil
    end

    if not registered then
        local port, registerError = sync.Register(SYNC_ID, SYNC_VERSION, onSyncOperation)
        if port == nil then
            initializationFailed = true
            log("Isaac.Sync.Register failed: " .. tostring(registerError))
            return nil
        end
        syncPort = port
        registered = true
        log("Isaac.Sync registered on derived port " .. tostring(syncPort))
    end

    if not startRequested then
        local ok, startError = sync.Start()
        if not ok then
            initializationFailed = true
            log("Isaac.Sync.Start failed: " .. tostring(startError))
            return nil
        end
        startRequested = true
        log("Isaac.Sync Start requested")
    end

    return sync
end

function KToTreasureRoom:OnPostUpdate()
    local sync = getSync()
    if sync == nil or not sync.IsActive() then
        activeSession = false
        lastTransitionFrame = nil
        return
    end

    if not activeSession then
        activeSession = true
        lastTransitionFrame = nil
        log("Isaac.Sync ACTIVE")
    end

    if not Input.IsButtonTriggered(Keyboard.KEY_K, 0) then
        return
    end

    local treasureRoomIndex = findTreasureRoomIndex()
    if treasureRoomIndex == nil then
        log("No Treasure Room exists on this floor.")
        return
    end

    -- This only queues a future deterministic frame. The local and remote
    -- games both transition later through onSyncOperation.
    local targetFrame, sendError = sync.Send(
        syncPort, OPCODE_GO_TO_TREASURE, treasureRoomIndex)
    if targetFrame == nil then
        log("Isaac.Sync.Send failed: " .. tostring(sendError))
        return
    end

    log("Queued Treasure Room transition for frame " .. tostring(targetFrame))
end

KToTreasureRoom:AddCallback(ModCallbacks.MC_POST_UPDATE, KToTreasureRoom.OnPostUpdate)
