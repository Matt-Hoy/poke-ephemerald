#include "global.h"
#include "main.h"
#include "malloc.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_ai_util.h"
#include "battle_ai_main.h"
#include "battle_controllers.h"
#include "battle_factory.h"
#include "battle_setup.h"
#include "battle_z_move.h"
#include "battle_terastal.h"
#include "data.h"
#include "debug.h"
#include "event_data.h"
#include "item.h"
#include "pokemon.h"
#include "random.h"
#include "recorded_battle.h"
#include "util.h"
#include "script.h"
#include "constants/abilities.h"
#include "constants/battle_ai.h"
#include "constants/battle_move_effects.h"
#include "constants/hold_effects.h"
#include "constants/moves.h"
#include "constants/items.h"
#include "constants/trainers.h"

#define AI_ACTION_DONE          (1 << 0)
#define AI_ACTION_FLEE          (1 << 1)
#define AI_ACTION_WATCH         (1 << 2)
#define AI_ACTION_DO_NOT_ATTACK (1 << 3)

static u32 ChooseMoveOrAction_Singles(u32 battlerAi);
static u32 ChooseMoveOrAction_Doubles(u32 battlerAi);
static s32 CalculateEngineMoveScore(u32 battlerAtk, u32 battlerDef, u32 move);
static inline void BattleAI_DoAIProcessing(struct AI_ThinkingStruct *aiThink, u32 battlerAi, u32 battlerDef);
static bool32 IsPinchBerryItemEffect(u32 holdEffect);

// ewram
EWRAM_DATA const u8 *gAIScriptPtr = NULL;   // Still used in contests
EWRAM_DATA AiScoreFunc sDynamicAiFunc = NULL;
EWRAM_DATA bool32 SUB_TEST_CONTROL = NULL;


// const rom data
static s32 AI_CheckBadMove(u32 battlerAtk, u32 battlerDef, u32 move, s32 score);
static s32 AI_TryToFaint(u32 battlerAtk, u32 battlerDef, u32 move);
static s32 AI_CheckViability(u32 battlerAtk, u32 battlerDef, u32 move, s32 score);
static u32 AI_CalcMoveEffectScore(u32 battlerAtk, u32 battlerDef, u32 move);

static s32 AI_DoubleBattle(u32 battlerAtk, u32 battlerDef, u32 move, s32 score);

// Sets up items for battle. Unchanged.
void BattleAI_SetupItems(void)
{
    s32 i;
    u8 *data = (u8 *)BATTLE_HISTORY;
    const u16 *items = GetTrainerItemsFromId(TRAINER_BATTLE_PARAM.opponentA);

    for (i = 0; i < sizeof(struct BattleHistory); i++)
        data[i] = 0;

    // Items are allowed to use in ONLY trainer battles.
    if ((gBattleTypeFlags & BATTLE_TYPE_TRAINER)
        && !(gBattleTypeFlags & (BATTLE_TYPE_LINK | BATTLE_TYPE_SAFARI | BATTLE_TYPE_BATTLE_TOWER
                               | BATTLE_TYPE_EREADER_TRAINER | BATTLE_TYPE_SECRET_BASE | BATTLE_TYPE_FRONTIER
                               | BATTLE_TYPE_INGAME_PARTNER | BATTLE_TYPE_RECORDED_LINK)
            )
       )
    {
        for (i = 0; i < MAX_TRAINER_ITEMS; i++)
        {
            if (items[i] != ITEM_NONE)
            {
                BATTLE_HISTORY->trainerItems[BATTLE_HISTORY->itemsNo] = items[i];
                BATTLE_HISTORY->itemsNo++;
            }
        }
    }
}

// Flags for wild mons. Unchanged.
static u32 GetWildAiFlags(void)
{
    u32 avgLevel = GetMonData(&gEnemyParty[0], MON_DATA_LEVEL);
    u32 flags = 0;

    if (IsDoubleBattle())
        avgLevel = (GetMonData(&gEnemyParty[0], MON_DATA_LEVEL) + GetMonData(&gEnemyParty[1], MON_DATA_LEVEL)) / 2;

    flags |= AI_FLAG_CHECK_BAD_MOVE;
    if (avgLevel >= 20)
        flags |= AI_FLAG_CHECK_VIABILITY;
    if (avgLevel >= 60)
        flags |= AI_FLAG_TRY_TO_2HKO;
    if (avgLevel >= 80)
        flags |= AI_FLAG_HP_AWARE;

    if (B_VAR_WILD_AI_FLAGS != 0 && VarGet(B_VAR_WILD_AI_FLAGS) != 0)
        flags |= VarGet(B_VAR_WILD_AI_FLAGS);

    return flags;
}

// Gets active flags and merges them into an int.
static u32 GetAiFlags(u16 trainerId)
{
    u32 flags = 0;

    if (!(gBattleTypeFlags & BATTLE_TYPE_HAS_AI) && !IsWildMonSmart())
        return 0;
    if (trainerId == 0xFFFF)
    {
        flags = GetWildAiFlags();
    }
    else
    {
        if (gBattleTypeFlags & BATTLE_TYPE_RECORDED)
            flags = GetAiScriptsInRecordedBattle(); // Don't know what this is
        else if (gBattleTypeFlags & (BATTLE_TYPE_FRONTIER | BATTLE_TYPE_EREADER_TRAINER | BATTLE_TYPE_TRAINER_HILL | BATTLE_TYPE_SECRET_BASE))
            flags = AI_FLAG_CHECK_BAD_MOVE | AI_FLAG_CHECK_VIABILITY | AI_FLAG_TRY_TO_FAINT;
    }

    return flags;
}

// Distributes flags per trainer to the 4 pokemon battle slots
// Will be unnecessary once battle AI no longer cares about flags.
void BattleAI_SetupFlags(void)
{
    if (IsAiVsAiBattle())
        AI_THINKING_STRUCT->aiFlags[B_POSITION_PLAYER_LEFT] = GetAiFlags(gPartnerTrainerId);
    else
        AI_THINKING_STRUCT->aiFlags[B_POSITION_PLAYER_LEFT] = 0; // player has no AI

    if (DEBUG_OVERWORLD_MENU && gIsDebugBattle)
    {
        AI_THINKING_STRUCT->aiFlags[B_POSITION_OPPONENT_LEFT] = gDebugAIFlags;
        AI_THINKING_STRUCT->aiFlags[B_POSITION_OPPONENT_RIGHT] = gDebugAIFlags;
        return;
    }

    if (IsWildMonSmart() && !(gBattleTypeFlags & (BATTLE_TYPE_LINK | BATTLE_TYPE_TRAINER)))
    {
        // smart wild AI
        AI_THINKING_STRUCT->aiFlags[B_POSITION_OPPONENT_LEFT] = GetAiFlags(0xFFFF);
        AI_THINKING_STRUCT->aiFlags[B_POSITION_OPPONENT_RIGHT] = GetAiFlags(0xFFFF);
    }
    else
    {
        AI_THINKING_STRUCT->aiFlags[B_POSITION_OPPONENT_LEFT] = GetAiFlags(TRAINER_BATTLE_PARAM.opponentA);
        if (TRAINER_BATTLE_PARAM.opponentB != 0)
            AI_THINKING_STRUCT->aiFlags[B_POSITION_OPPONENT_RIGHT] = GetAiFlags(TRAINER_BATTLE_PARAM.opponentB);
        else
            AI_THINKING_STRUCT->aiFlags[B_POSITION_OPPONENT_RIGHT] = AI_THINKING_STRUCT->aiFlags[B_POSITION_OPPONENT_LEFT];
    }

    if (gBattleTypeFlags & BATTLE_TYPE_INGAME_PARTNER)
    {
        AI_THINKING_STRUCT->aiFlags[B_POSITION_PLAYER_RIGHT] = GetAiFlags(gPartnerTrainerId - TRAINER_PARTNER(PARTNER_NONE));
    }
    else if (IsDoubleBattle() && IsAiVsAiBattle())
    {
        AI_THINKING_STRUCT->aiFlags[B_POSITION_PLAYER_RIGHT] = AI_THINKING_STRUCT->aiFlags[B_POSITION_PLAYER_LEFT];
    }
    else
    {
        AI_THINKING_STRUCT->aiFlags[B_POSITION_PLAYER_RIGHT] = 0; // player
    }
}

// Setus up basic data for the ai
void BattleAI_SetupAIData(u8 defaultScoreMoves, u32 battler)
{
    s32 i;
    u8 moveLimitations;
    u32 flags[MAX_BATTLERS_COUNT];

    // Clear AI data but preserve the flags.
    memcpy(&flags[0], &AI_THINKING_STRUCT->aiFlags[0], sizeof(u32) * MAX_BATTLERS_COUNT);
    memset(AI_THINKING_STRUCT, 0, sizeof(struct AI_ThinkingStruct));
    memcpy(&AI_THINKING_STRUCT->aiFlags[0], &flags[0], sizeof(u32) * MAX_BATTLERS_COUNT);

    // Conditional score reset, unlike Ruby.
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        if (defaultScoreMoves & 1)
            SET_SCORE(battler, i, AI_SCORE_DEFAULT);
        else
            SET_SCORE(battler, i, 0);

        defaultScoreMoves >>= 1;
    }

    moveLimitations = AI_DATA->moveLimitations[battler];

    // Ignore moves that aren't possible to use.
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        if ((1u << i) & moveLimitations)
            SET_SCORE(battler, i, 0);
    }

    gBattlerTarget = SetRandomTarget(battler);
    gBattleStruct->aiChosenTarget[battler] = gBattlerTarget;
}

// Chooses move or action based on each battler
u32 BattleAI_ChooseMoveOrAction(u32 battler)
{
    // DebugPrintf("Entering BattleAI_ChooseMoveOrAction");
    // DebugPrintf("Ability: %d", AI_DATA->abilities[gBattlerTarget]);
    Ai_InitPartyStruct();
    u32 ret;

    if (!IsDoubleBattle())
        ret = ChooseMoveOrAction_Singles(battler);
    else
        ret = ChooseMoveOrAction_Doubles(battler);

    // DebugPrintf("Checkpoint5");
    // Clear protect structures, some flags may be set during AI calcs
    // e.g. pranksterElevated from GetBattleMovePriority
    memset(&gProtectStructs, 0, MAX_BATTLERS_COUNT * sizeof(struct ProtectStruct));
    #if TESTING
    TestRunner_Battle_CheckAiMoveScores(battler);
    #endif // TESTING

    // DebugPrintf("Checkpoint6");
    return ret;
}

static void CopyBattlerDataToAIParty(u32 bPosition, u32 side)
{
    u32 battler = GetBattlerAtPosition(bPosition);
    struct AiPartyMon *aiMon = &AI_PARTY->mons[side][gBattlerPartyIndexes[battler]];
    struct BattlePokemon *bMon = &gBattleMons[battler];

    aiMon->species = bMon->species;
    aiMon->level = bMon->level;
    aiMon->status = bMon->status1;
    aiMon->gender = GetBattlerGender(battler);
    aiMon->isFainted = FALSE;
    aiMon->wasSentInBattle = TRUE;
    aiMon->switchInCount++;
}

void Ai_InitPartyStruct(void)
{
    u32 i;
    struct Pokemon *mon;

    AI_PARTY->count[B_SIDE_PLAYER] = CalculatePlayerPartyCount();
    AI_PARTY->count[B_SIDE_OPPONENT] = gEnemyPartyCount;

    // Save first 2 or 4(in doubles) mons
    CopyBattlerDataToAIParty(B_POSITION_PLAYER_LEFT, B_SIDE_PLAYER);
    if (IsDoubleBattle())
        CopyBattlerDataToAIParty(B_POSITION_PLAYER_RIGHT, B_SIDE_PLAYER);

    // If player's partner is AI, save opponent mons
    if (gBattleTypeFlags & BATTLE_TYPE_INGAME_PARTNER)
    {
        CopyBattlerDataToAIParty(B_POSITION_OPPONENT_LEFT, B_SIDE_OPPONENT);
        CopyBattlerDataToAIParty(B_POSITION_OPPONENT_RIGHT, B_SIDE_OPPONENT);
    }

    // Find fainted mons & Get mon data
    for (i = 0; i < AI_PARTY->count[B_SIDE_PLAYER]; i++)
    {
        if (GetMonData(&gPlayerParty[i], MON_DATA_HP) == 0)
            AI_PARTY->mons[B_SIDE_PLAYER][i].isFainted = TRUE;

        u32 j;
        mon = &gPlayerParty[i];
        AI_PARTY->mons[B_SIDE_PLAYER][i].item = GetMonData(mon, MON_DATA_HELD_ITEM);
        AI_PARTY->mons[B_SIDE_PLAYER][i].heldEffect = ItemId_GetHoldEffect(AI_PARTY->mons[B_SIDE_PLAYER][i].item);
        AI_PARTY->mons[B_SIDE_PLAYER][i].ability = GetMonAbility(mon);
        for (j = 0; j < MAX_MON_MOVES; j++)
            AI_PARTY->mons[B_SIDE_PLAYER][i].moves[j] = GetMonData(mon, MON_DATA_MOVE1 + j);
    }
}

void Ai_UpdateSwitchInData(u32 battler)
{
    u32 i;
    u32 side = GetBattlerSide(battler);
    struct AiPartyMon *aiMon = &AI_PARTY->mons[side][gBattlerPartyIndexes[battler]];

    // See if the switched-in mon has already been in battle
    if (aiMon->wasSentInBattle)
    {
        if (aiMon->ability)
            BATTLE_HISTORY->abilities[battler] = aiMon->ability;
        if (aiMon->heldEffect)
            BATTLE_HISTORY->itemEffects[battler] = aiMon->heldEffect;
        for (i = 0; i < MAX_MON_MOVES; i++)
        {
            if (aiMon->moves[i])
                BATTLE_HISTORY->usedMoves[battler][i] = aiMon->moves[i];
        }
        aiMon->switchInCount++;
        aiMon->status = gBattleMons[battler].status1; // Copy status, because it could've been changed in battle.
    }
    else // If not, copy the newly switched-in mon in battle and clear battle history.
    {
        ClearBattlerMoveHistory(battler);
        ClearBattlerAbilityHistory(battler);
        ClearBattlerItemEffectHistory(battler);
        CopyBattlerDataToAIParty(GetBattlerPosition(battler), side);
    }
}

void Ai_UpdateFaintData(u32 battler)
{
    struct AiPartyMon *aiMon = &AI_PARTY->mons[GetBattlerSide(battler)][gBattlerPartyIndexes[battler]];
    ClearBattlerMoveHistory(battler);
    ClearBattlerAbilityHistory(battler);
    ClearBattlerItemEffectHistory(battler);
    aiMon->isFainted = TRUE;
}

void SetBattlerAiData(u32 battler, struct AiLogicData *aiData)
{
    u32 ability, holdEffect;

    ability = aiData->abilities[battler] = AI_DecideKnownAbilityForTurn(battler);
    aiData->items[battler] = gBattleMons[battler].item;
    holdEffect = aiData->holdEffects[battler] = AI_DecideHoldEffectForTurn(battler);
    aiData->holdEffectParams[battler] = GetBattlerHoldEffectParam(battler);
    aiData->lastUsedMove[battler] = gLastMoves[battler];
    aiData->hpPercents[battler] = GetHealthPercentage(battler);
    aiData->moveLimitations[battler] = CheckMoveLimitations(battler, 0, MOVE_LIMITATIONS_ALL);
    aiData->speedStats[battler] = GetBattlerTotalSpeedStatArgs(battler, ability, holdEffect);
}

static u32 Ai_SetMoveAccuracy(struct AiLogicData *aiData, u32 battlerAtk, u32 battlerDef, u32 move)
{
    u32 accuracy;
    u32 abilityAtk = aiData->abilities[battlerAtk];
    u32 abilityDef = aiData->abilities[battlerDef];
    if (abilityAtk == ABILITY_NO_GUARD || abilityDef == ABILITY_NO_GUARD || GetMoveAccuracy(move) == 0) // Moves with accuracy 0 or no guard ability always hit.
        accuracy = 100;
    else
        accuracy = GetTotalAccuracy(battlerAtk, battlerDef, move, abilityAtk, abilityDef, aiData->holdEffects[battlerAtk], aiData->holdEffects[battlerDef]);

    return accuracy;
}

static void CalcBattlerAiMovesData(struct AiLogicData *aiData, u32 battlerAtk, u32 battlerDef, u32 weather)
{
    u32 moveIndex, move;
    u32 rollType = GetDmgRollType(battlerAtk);
    u16 *moves = GetMovesArray(battlerAtk);

    for (moveIndex = 0; moveIndex < MAX_MON_MOVES; moveIndex++)
    {
        struct SimulatedDamage dmg = {0};
        uq4_12_t effectiveness = Q_4_12(0.0);
        move = moves[moveIndex];

        // DebugPrintf("SIM MOVE: %d", move);
        if (move != MOVE_NONE
            && move != MOVE_UNAVAILABLE
            //&& !IsBattleMoveStatus(move)  /* we want to get effectiveness and accuracy of status moves */
            && !(aiData->moveLimitations[battlerAtk] & (1u << moveIndex)))
        {
            dmg = AI_CalcDamage(move, battlerAtk, battlerDef, &effectiveness, TRUE, weather, rollType);
            // DebugPrintf("Simulated DMG: %d", dmg);
            aiData->moveAccuracy[battlerAtk][battlerDef][moveIndex] = Ai_SetMoveAccuracy(aiData, battlerAtk, battlerDef, move);
        }
        aiData->simulatedDmg[battlerAtk][battlerDef][moveIndex] = dmg;
        aiData->effectiveness[battlerAtk][battlerDef][moveIndex] = effectiveness;
    }
}

static void SetBattlerAiMovesData(struct AiLogicData *aiData, u32 battlerAtk, u32 battlersCount, u32 weather)
{
    u32 battlerDef;
    SaveBattlerData(battlerAtk);
    SetBattlerData(battlerAtk);

    // Simulate dmg for both ai controlled mons and for player controlled mons.
    for (battlerDef = 0; battlerDef < battlersCount; battlerDef++)
    {
        if (battlerAtk == battlerDef || !IsBattlerAlive(battlerDef))
            continue;

        SaveBattlerData(battlerDef);
        SetBattlerData(battlerDef);
        CalcBattlerAiMovesData(aiData, battlerAtk, battlerDef, weather);
        RestoreBattlerData(battlerDef);
    }
    RestoreBattlerData(battlerAtk);
}

void SetAiLogicDataForTurn(struct AiLogicData *aiData)
{
    u32 battlerAtk, battlersCount, weather;

    memset(aiData, 0, sizeof(struct AiLogicData));
    if (!(gBattleTypeFlags & BATTLE_TYPE_HAS_AI) && !IsWildMonSmart())
        return;

    // Set delay timer to count how long it takes for AI to choose action/move
    gBattleStruct->aiDelayTimer = gMain.vblankCounter1;

    aiData->weatherHasEffect = HasWeatherEffect();
    weather = AI_GetWeather();

    // get/assume all battler data and simulate AI damage
    battlersCount = gBattlersCount;

    AI_DATA->aiCalcInProgress = TRUE;
    if (DEBUG_AI_DELAY_TIMER)
        CycleCountStart();
    for (battlerAtk = 0; battlerAtk < battlersCount; battlerAtk++)
    {
        if (!IsBattlerAlive(battlerAtk))
            continue;

        SetBattlerAiData(battlerAtk, aiData);
    }

    for (battlerAtk = 0; battlerAtk < battlersCount; battlerAtk++)
    {
        if (!IsBattlerAlive(battlerAtk))
            continue;

        SetBattlerAiMovesData(aiData, battlerAtk, battlersCount, weather);
    }
    if (DEBUG_AI_DELAY_TIMER)
        // We add to existing to compound multiple calls
        gBattleStruct->aiDelayCycles += CycleCountEnd();
    AI_DATA->aiCalcInProgress = FALSE;
}

static u32 ChooseMoveOrAction_Singles(u32 battlerAi)
{
    // DebugPrintf("Entering ChooseMoveOrAction_Singles");
    s16 currentMoveArray[MAX_MON_MOVES];
    s16 consideredMoveArray[MAX_MON_MOVES];
    u32 numOfBestMoves;
    s32 i;

    AI_DATA->partnerMove = 0;   // no ally

    if (gBattleTypeFlags & BATTLE_TYPE_SAFARI)
    {
        u32 safariFleeRate = gBattleStruct->safariEscapeFactor * 5; // Safari flee rate, from 0-20.
        if ((Random() % 100) < safariFleeRate)
            AI_THINKING_STRUCT->aiAction |= (AI_ACTION_DONE | AI_ACTION_FLEE | AI_ACTION_DO_NOT_ATTACK);
        else
            AI_THINKING_STRUCT->aiAction |= (AI_ACTION_DONE | AI_ACTION_WATCH | AI_ACTION_DO_NOT_ATTACK);
    }
    else if (gBattleTypeFlags & BATTLE_TYPE_ROAMER)
        AI_THINKING_STRUCT->aiAction |= (AI_ACTION_DONE | AI_ACTION_FLEE | AI_ACTION_DO_NOT_ATTACK);
    else if (gBattleTypeFlags & BATTLE_TYPE_FIRST_BATTLE)
    {
        if (AI_DATA->hpPercents[gBattlerTarget] <= 20)
            AI_THINKING_STRUCT->aiAction |= (AI_ACTION_DONE | AI_ACTION_FLEE | AI_ACTION_DO_NOT_ATTACK);
    }

    BattleAI_DoAIProcessing(AI_THINKING_STRUCT, battlerAi, gBattlerTarget);

    // DebugPrintf("Checkpoint1");
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        gBattleStruct->aiFinalScore[battlerAi][gBattlerTarget][i] = AI_THINKING_STRUCT->score[i];
        // DebugPrintf("Index: %d", i);
        // DebugPrintf("Score: %d", AI_THINKING_STRUCT->score[i]);
    }

    // Check special AI actions.
    if (AI_THINKING_STRUCT->aiAction & AI_ACTION_FLEE)
        return AI_CHOICE_FLEE;
    if (AI_THINKING_STRUCT->aiAction & AI_ACTION_WATCH)
        return AI_CHOICE_WATCH;

    // DebugPrintf("Checkpoint2");
    numOfBestMoves = 1;
    // DebugPrintf("INITIALIZER Score: %d", AI_THINKING_STRUCT->score[0]);
    currentMoveArray[0] = AI_THINKING_STRUCT->score[0];
    // DebugPrintf("ASSIGNED sc%d", currentMoveArray[0]);

    // DebugPrintf("Checkpoint3");
    consideredMoveArray[0] = 0;

    // find and choose the highest scoring move. In the case of a tie, choose randomly.
    for (i = 1; i < MAX_MON_MOVES; i++)
    {
        if (gBattleMons[battlerAi].moves[i] != MOVE_NONE)
        {
            // DebugPrintf("%d", currentMoveArray[0]);
            if (currentMoveArray[0] == AI_THINKING_STRUCT->score[i])
            {
                currentMoveArray[numOfBestMoves] = AI_THINKING_STRUCT->score[i];
                consideredMoveArray[numOfBestMoves++] = i;
            }
            if (currentMoveArray[0] < AI_THINKING_STRUCT->score[i])
            {
                numOfBestMoves = 1;
                currentMoveArray[0] = AI_THINKING_STRUCT->score[i];
                consideredMoveArray[0] = i;
            }
        }
        // DebugPrintf("Move: %d", gBattleMons[battlerAi].moves[i]);
        // DebugPrintf("numOfBestMoves: %d", numOfBestMoves);
    }
    
    // DebugPrintf("Checkpoint4");
    return consideredMoveArray[Random() % numOfBestMoves];
}

static u32 ChooseMoveOrAction_Doubles(u32 battlerAi)
{
    // DebugPrintf("Entering ChooseMoveOrAction_Doubles");
    s32 i, j;
    s32 bestMovePointsForTarget[MAX_BATTLERS_COUNT];
    u8 mostViableTargetsArray[MAX_BATTLERS_COUNT];
    u8 actionOrMoveIndex[MAX_BATTLERS_COUNT];
    s32 mostViableMovesScores[MAX_MON_MOVES];
    u8 mostViableMovesIndices[MAX_MON_MOVES];
    u32 mostViableTargetsNo;
    u32 mostViableMovesNo;
    s32 mostMovePoints;

    for (i = 0; i < MAX_BATTLERS_COUNT; i++)
    {
        if (i == battlerAi || gBattleMons[i].hp == 0)
        {
            actionOrMoveIndex[i] = 0xFF;
            bestMovePointsForTarget[i] = -1;
        }
        else
        {
            if (gBattleTypeFlags & BATTLE_TYPE_PALACE)
                BattleAI_SetupAIData(gBattleStruct->palaceFlags >> 4, battlerAi);
            else
                BattleAI_SetupAIData(0xF, battlerAi);

            gBattlerTarget = i;

            AI_DATA->partnerMove = GetAllyChosenMove(battlerAi);
            AI_THINKING_STRUCT->aiLogicId = 0;
            AI_THINKING_STRUCT->movesetIndex = 0;

            BattleAI_DoAIProcessing(AI_THINKING_STRUCT, battlerAi, gBattlerTarget);

            if (AI_THINKING_STRUCT->aiAction & AI_ACTION_FLEE)
            {
                actionOrMoveIndex[i] = AI_CHOICE_FLEE;
            }
            else if (AI_THINKING_STRUCT->aiAction & AI_ACTION_WATCH)
            {
                actionOrMoveIndex[i] = AI_CHOICE_WATCH;
            }
            else
            {
                mostViableMovesScores[0] = AI_THINKING_STRUCT->score[0];
                mostViableMovesIndices[0] = 0;
                mostViableMovesNo = 1;
                for (j = 1; j < MAX_MON_MOVES; j++)
                {
                    // DebugPrintf("Index: %d", j);
                    // DebugPrintf("Doubles Move Score: %d", AI_THINKING_STRUCT->score[j]);
                    if (gBattleMons[battlerAi].moves[j] != 0)
                    {
                        if (!CanTargetBattler(battlerAi, i, gBattleMons[battlerAi].moves[j]))
                            continue;

                        if (mostViableMovesScores[0] == AI_THINKING_STRUCT->score[j])
                        {
                            mostViableMovesScores[mostViableMovesNo] = AI_THINKING_STRUCT->score[j];
                            mostViableMovesIndices[mostViableMovesNo] = j;
                            mostViableMovesNo++;
                        }
                        if (mostViableMovesScores[0] < AI_THINKING_STRUCT->score[j])
                        {
                            mostViableMovesScores[0] = AI_THINKING_STRUCT->score[j];
                            mostViableMovesIndices[0] = j;
                            mostViableMovesNo = 1;
                        }
                    }
                }
                actionOrMoveIndex[i] = mostViableMovesIndices[Random() % mostViableMovesNo];
                bestMovePointsForTarget[i] = mostViableMovesScores[0];
            }

            for (j = 0; j < MAX_MON_MOVES; j++)
            {
                gBattleStruct->aiFinalScore[battlerAi][gBattlerTarget][j] = AI_THINKING_STRUCT->score[j];
            }
        }
    }

    mostMovePoints = bestMovePointsForTarget[0];
    mostViableTargetsArray[0] = 0;
    mostViableTargetsNo = 1;

    for (i = 1; i < MAX_BATTLERS_COUNT; i++)
    {
        if (mostMovePoints == bestMovePointsForTarget[i])
        {
            mostViableTargetsArray[mostViableTargetsNo] = i;
            mostViableTargetsNo++;
        }
        if (mostMovePoints < bestMovePointsForTarget[i])
        {
            mostMovePoints = bestMovePointsForTarget[i];
            mostViableTargetsArray[0] = i;
            mostViableTargetsNo = 1;
        }
    }

    gBattlerTarget = mostViableTargetsArray[Random() % mostViableTargetsNo];
    gBattleStruct->aiChosenTarget[battlerAi] = gBattlerTarget;
    return actionOrMoveIndex[gBattlerTarget];
}

static inline bool32 ShouldConsiderMoveForBattler(u32 battlerAi, u32 battlerDef, u32 move)
{
    // If the AI's partner is the target of the attack
    if (battlerAi == BATTLE_PARTNER(battlerDef))
    {
        u32 target = GetBattlerMoveTargetType(battlerAi, move);
        // If the move is meant to target both opponents or the opponent's field, return FALSE
        if (target == MOVE_TARGET_BOTH || target == MOVE_TARGET_OPPONENTS_FIELD)
            return FALSE;
    }
    return TRUE;
}

static inline void BattleAI_DoAIProcessing(struct AI_ThinkingStruct *aiThink, u32 battlerAi, u32 battlerDef)
{
    // DebugPrintf("Entering BattleAI_DoAIProcessing");
    do
    {
        // DebugPrintf("Beginning Loop");
        // If the given move has PP, consider a move
        if (gBattleMons[battlerAi].pp[aiThink->movesetIndex] == 0)
        {
            // DebugPrintf("No PP");
            aiThink->moveConsidered = MOVE_NONE;
        }
        else
        {
            // DebugPrintf("MoveHasPP");
            aiThink->moveConsidered = gBattleMons[battlerAi].moves[aiThink->movesetIndex];
            // DebugPrintf("Move Considered???: %d", aiThink->moveConsidered);
            // DebugPrintf("Move Priority: %d", GetBattleMovePriority(battlerAi, aiThink->moveConsidered));
        }

        // DebugPrintf("Considering?");
        if (aiThink->moveConsidered != MOVE_NONE
          && aiThink->score[aiThink->movesetIndex] > 0
          && ShouldConsiderMoveForBattler(battlerAi, battlerDef, aiThink->moveConsidered))
        {
            aiThink->score[aiThink->movesetIndex] = CalculateEngineMoveScore(battlerAi, 
                battlerDef, 
                aiThink->moveConsidered);
            // DebugPrintf("Final Score: %d\n", aiThink->score[aiThink->movesetIndex]);
        }
        else if (sDynamicAiFunc != NULL)
            aiThink->score[aiThink->movesetIndex] = sDynamicAiFunc(battlerAi,
                battlerDef,
                aiThink->moveConsidered,
                0);
        else
        {
            aiThink->score[aiThink->movesetIndex] = 0;
            // DebugPrintf("Zeroing?");
        }
        aiThink->movesetIndex++;
        // DebugPrintf("Looping?");
    } while (aiThink->movesetIndex < MAX_MON_MOVES && !(aiThink->aiAction & AI_ACTION_DO_NOT_ATTACK));
    
}

static s32 CalculateEngineMoveScore(u32 battlerAtk, u32 battlerDef, u32 move)
{
    // DebugPrintf("=================================");
    // DebugPrintf("Move Considered: %d", move);
    CalcBattlerAiMovesData(AI_DATA, battlerAtk, battlerDef, AI_GetWeather());
    s32 finalScore = AI_CheckBadMove(battlerAtk, battlerDef, move, 0);
    if (finalScore < 0)
    {
        // DebugPrintf("Not checking subsequent functions: %d", finalScore);
        return finalScore;
    }
    finalScore += AI_TryToFaint(battlerAtk, battlerDef, move);
    // DebugPrintf("finalScore after TrytoFaint: %d", finalScore);
    finalScore = AI_CheckViability(battlerAtk, battlerDef, move, finalScore);
    // DebugPrintf("finalScore after CheckViability: %d", finalScore);
    if (IsValidDoubleBattle(battlerAtk))
        finalScore = AI_DoubleBattle(battlerAtk, battlerDef, move, finalScore);
    
    // DebugPrintf("finalScore after DoubleBattle: %d", finalScore);
    return finalScore;
}

// AI Score Functions
// AI_FLAG_CHECK_BAD_MOVE - decreases move scores
static s32 AI_CheckBadMove(u32 battlerAtk, u32 battlerDef, u32 move, s32 score)
{
    if (move == MOVE_CELEBRATE)
        RETURN_SCORE_MINUS(10);
    // DebugPrintf("Entering AI_CheckBadMove");
    // move data
    s8 atkPriority = GetBattleMovePriority(battlerAtk, move);
    u32 moveEffect = GetMoveEffect(move);
    s32 moveType;
    u32 moveTarget = GetBattlerMoveTargetType(battlerAtk, move);
    struct AiLogicData *aiData = AI_DATA;
    uq4_12_t effectiveness = aiData->effectiveness[battlerAtk][battlerDef][AI_THINKING_STRUCT->movesetIndex];
    bool32 isDoubleBattle = IsValidDoubleBattle(battlerAtk);
    u32 i;
    u32 weather;
    u32 predictedMove = aiData->lastUsedMove[battlerDef];

    if (IS_TARGETING_PARTNER(battlerAtk, battlerDef) && !IsBattleMoveStatus(move))
    {
        // DebugPrintf("Is Targeting Partner");
        RETURN_SCORE_MINUS(20);
    }

    SetTypeBeforeUsingMove(move, battlerAtk);
    moveType = GetBattleMoveType(move);

    if (IsPowderMove(move) && !IsAffectedByPowder(battlerDef, aiData->abilities[battlerDef], aiData->holdEffects[battlerDef]))
        RETURN_SCORE_MINUS(20);

    if (IsSemiInvulnerable(battlerDef, move) && moveEffect != EFFECT_SEMI_INVULNERABLE && AI_IsFaster(battlerAtk, battlerDef, move))
        RETURN_SCORE_MINUS(10);

    if (IsTwoTurnNotSemiInvulnerableMove(battlerAtk, move) && CanTargetFaintAi(battlerDef, battlerAtk))
        RETURN_SCORE_MINUS(10);

    if (gBattleStruct->battlerState[battlerDef].commandingDondozo)
        RETURN_SCORE_MINUS(20);

    if (effectiveness == UQ_4_12(0.0))
    {
        RETURN_SCORE_MINUS(20);
    }
    else if (effectiveness < UQ_4_12(0.5))
    {
        switch (moveEffect)
        {
        case EFFECT_FIXED_DAMAGE_ARG:
        case EFFECT_LEVEL_DAMAGE:
        case EFFECT_PSYWAVE:
        case EFFECT_OHKO:
        case EFFECT_BIDE:
        case EFFECT_SUPER_FANG:
        case EFFECT_ENDEAVOR:
        case EFFECT_COUNTER:
        case EFFECT_MIRROR_COAT:
        case EFFECT_METAL_BURST:
        case EFFECT_FINAL_GAMBIT:
        case EFFECT_GUARDIAN_OF_ALOLA:
            break;
        default:
            RETURN_SCORE_MINUS(10);
        }
    }

    // check non-user target
    if (!(moveTarget & MOVE_TARGET_USER))
    {
        // DebugPrintf("if (!(moveTarget & MOVE_TARGET_USER))");
        // DebugPrintf("Actual Priority: %d", GetBattleMovePriority(battlerAtk, move));
        // target ability checks
        if (!DoesBattlerIgnoreAbilityChecks(battlerAtk, aiData->abilities[battlerAtk], move))
        {
            // DebugPrintf("Not blocked yet? %d", CanAbilityBlockMove(battlerAtk, battlerDef, move, aiData->abilities[battlerDef], ABILITY_CHECK_TRIGGER)); 
            if (CanAbilityBlockMove(battlerAtk, battlerDef, move, aiData->abilities[battlerDef], ABILITY_CHECK_TRIGGER))
            {
                // DebugPrintf("Blocked? %d", CanAbilityBlockMove(battlerAtk, battlerDef, move, aiData->abilities[battlerDef], ABILITY_CHECK_TRIGGER));
                RETURN_SCORE_MINUS(20);
            }
            if (CanAbilityAbsorbMove(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, moveType, ABILITY_CHECK_TRIGGER))
                RETURN_SCORE_MINUS(20);
            // DebugPrintf("Here");
            
            switch (aiData->abilities[battlerDef])
            {
            case ABILITY_MAGIC_GUARD:
                switch (moveEffect)
                {
                case EFFECT_POISON:
                case EFFECT_WILL_O_WISP:
                case EFFECT_TOXIC:
                case EFFECT_LEECH_SEED:
                    RETURN_SCORE_MINUS(5);
                    break;
                case EFFECT_CURSE:
                    if (IS_BATTLER_OF_TYPE(battlerAtk, TYPE_GHOST)) // Don't use Curse if you're a ghost type vs a Magic Guard user, they'll take no damage.
                        RETURN_SCORE_MINUS(5);
                    break;
                }
                break;
            case ABILITY_WONDER_GUARD:
                if (effectiveness < UQ_4_12(2.0))
                    RETURN_SCORE_MINUS(20);
                break;
            case ABILITY_JUSTIFIED:
                if (moveType == TYPE_DARK && !IsBattleMoveStatus(move))
                    RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_RATTLED:
                if (!IsBattleMoveStatus(move)
                  && (moveType == TYPE_DARK || moveType == TYPE_GHOST || moveType == TYPE_BUG))
                    RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_AROMA_VEIL:
                if (IsAromaVeilProtectedEffect(moveEffect))
                    RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_SWEET_VEIL:
                if (moveEffect == EFFECT_SLEEP || moveEffect == EFFECT_YAWN)
                    RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_FLOWER_VEIL:
                if (IS_BATTLER_OF_TYPE(battlerDef, TYPE_GRASS) && (IsNonVolatileStatusMoveEffect(moveEffect) || IsStatLoweringEffect(moveEffect)))
                    RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_MAGIC_BOUNCE:
                if (MoveCanBeBouncedBack(move))
                    RETURN_SCORE_MINUS(20);
                break;
            case ABILITY_CONTRARY:
                if (IsStatLoweringEffect(moveEffect))
                    RETURN_SCORE_MINUS(20);
                break;
            case ABILITY_CLEAR_BODY:
            case ABILITY_FULL_METAL_BODY:
            case ABILITY_WHITE_SMOKE:
                if (IsStatLoweringEffect(moveEffect))
                    RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_HYPER_CUTTER:
                if ((moveEffect == EFFECT_ATTACK_DOWN ||  moveEffect == EFFECT_ATTACK_DOWN_2)
                  && move != MOVE_PLAY_NICE && move != MOVE_NOBLE_ROAR && move != MOVE_TEARFUL_LOOK && move != MOVE_VENOM_DRENCH)
                    RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_ILLUMINATE:
                if (B_ILLUMINATE_EFFECT < GEN_9)
                    break;
                // fallthrough
            case ABILITY_KEEN_EYE:
            case ABILITY_MINDS_EYE:
                if (moveEffect == EFFECT_ACCURACY_DOWN || moveEffect == EFFECT_ACCURACY_DOWN_2)
                    RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_BIG_PECKS:
                if (moveEffect == EFFECT_DEFENSE_DOWN || moveEffect == EFFECT_DEFENSE_DOWN_2)
                    RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_DEFIANT:
            case ABILITY_COMPETITIVE:
                if (IsStatLoweringEffect(moveEffect) && !IS_TARGETING_PARTNER(battlerAtk, battlerDef))
                    RETURN_SCORE_MINUS(8);
                break;
            case ABILITY_COMATOSE:
                if (IsNonVolatileStatusMoveEffect(moveEffect))
                    RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_SHIELDS_DOWN:
                if (IsShieldsDownProtected(battlerAtk, aiData->abilities[battlerAtk]) && IsNonVolatileStatusMoveEffect(moveEffect))
                    RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_LEAF_GUARD:
                if ((AI_GetWeather() & B_WEATHER_SUN)
                  && aiData->holdEffects[battlerDef] != HOLD_EFFECT_UTILITY_UMBRELLA
                  && IsNonVolatileStatusMoveEffect(moveEffect))
                    RETURN_SCORE_MINUS(10);
                break;
            } // def ability checks

            // target partner ability checks & not attacking partner
            if (isDoubleBattle)
            {
                switch (aiData->abilities[BATTLE_PARTNER(battlerDef)])
                {
                case ABILITY_LIGHTNING_ROD:
                    if (moveType == TYPE_ELECTRIC && !IsMoveRedirectionPrevented(battlerAtk, move, aiData->abilities[battlerAtk]))
                        RETURN_SCORE_MINUS(20);
                    break;
                case ABILITY_STORM_DRAIN:
                    if (moveType == TYPE_WATER && !IsMoveRedirectionPrevented(battlerAtk, move, aiData->abilities[battlerAtk]))
                        RETURN_SCORE_MINUS(20);
                    break;
                case ABILITY_MAGIC_BOUNCE:
                    if (MoveCanBeBouncedBack(move) && moveTarget & (MOVE_TARGET_BOTH | MOVE_TARGET_FOES_AND_ALLY | MOVE_TARGET_OPPONENTS_FIELD))
                        RETURN_SCORE_MINUS(20);
                    break;
                case ABILITY_SWEET_VEIL:
                    if (moveEffect == EFFECT_SLEEP || moveEffect == EFFECT_YAWN)
                        RETURN_SCORE_MINUS(20);
                    break;
                case ABILITY_FLOWER_VEIL:
                    if ((IS_BATTLER_OF_TYPE(battlerDef, TYPE_GRASS)) && (IsNonVolatileStatusMoveEffect(moveEffect) || IsStatLoweringEffect(moveEffect)))
                        RETURN_SCORE_MINUS(10);
                    break;
                case ABILITY_AROMA_VEIL:
                    if (IsAromaVeilProtectedEffect(moveEffect))
                        RETURN_SCORE_MINUS(10);
                    break;
                }

                if (GetBattlerMoveTargetType(battlerAtk, move) == MOVE_TARGET_FOES_AND_ALLY 
                && IsBattlerAlive(BATTLE_PARTNER(battlerAtk)) 
                && aiData->abilities[BATTLE_PARTNER(battlerAtk)] != ABILITY_TELEPATHY
                && aiData->effectiveness[battlerAtk][BATTLE_PARTNER(battlerAtk)][AI_THINKING_STRUCT->movesetIndex] != 0.0)
                {
                    switch (aiData->abilities[BATTLE_PARTNER(battlerAtk)])
                    {
                        case ABILITY_LIGHTNING_ROD:
                            if (moveType != TYPE_ELECTRIC)
                                RETURN_SCORE_MINUS(3);
                            break;
                        case ABILITY_STORM_DRAIN:
                            if (moveType != TYPE_WATER)
                                RETURN_SCORE_MINUS(3);
                            break;
                        default:
                            RETURN_SCORE_MINUS(3);
                    }
                }
            } // def partner ability checks
        } // ignore def ability check

        // gen7+ dark type mons immune to priority->elevated moves from prankster
        if (B_PRANKSTER_DARK_TYPES >= GEN_7 && IS_BATTLER_OF_TYPE(battlerDef, TYPE_DARK)
          && aiData->abilities[battlerAtk] == ABILITY_PRANKSTER && IsBattleMoveStatus(move)
          && !(moveTarget & (MOVE_TARGET_OPPONENTS_FIELD | MOVE_TARGET_USER)))
            RETURN_SCORE_MINUS(10);

        // terrain & effect checks
        if (IsBattlerTerrainAffected(battlerDef, STATUS_FIELD_ELECTRIC_TERRAIN))
        {
            if (moveEffect == EFFECT_SLEEP || moveEffect == EFFECT_YAWN)
                RETURN_SCORE_MINUS(20);
        }

        if (IsBattlerTerrainAffected(battlerDef, STATUS_FIELD_MISTY_TERRAIN))
        {
            if (IsNonVolatileStatusMoveEffect(moveEffect) || IsConfusionMoveEffect(moveEffect))
                RETURN_SCORE_MINUS(20);
        }

        if (IsBattlerTerrainAffected(battlerAtk, STATUS_FIELD_PSYCHIC_TERRAIN) && atkPriority > 0)
        {
            RETURN_SCORE_MINUS(20);
        }
    } // end check MOVE_TARGET_USER

// the following checks apply to any target (including user)

    // throat chop check
    if (gDisableStructs[battlerAtk].throatChopTimer && IsSoundMove(move))
        return -100; // Can't even select move at all
    // heal block check
    if (gStatuses3[battlerAtk] & STATUS3_HEAL_BLOCK && IsHealBlockPreventingMove(battlerAtk, move))
        return -100; // Can't even select heal blocked move

    // primal weather check
    weather = AI_GetWeather();
    if (weather & B_WEATHER_PRIMAL_ANY)
    {
        switch (moveEffect)
        {
            case EFFECT_SUNNY_DAY:
            case EFFECT_RAIN_DANCE:
            case EFFECT_HAIL:
            case EFFECT_SNOWSCAPE:
            case EFFECT_SANDSTORM:
                RETURN_SCORE_MINUS(30);
        }

        if (!IsBattleMoveStatus(move))
        {
            if (weather & B_WEATHER_SUN_PRIMAL)
            {
                if (moveType == TYPE_WATER)
                    RETURN_SCORE_MINUS(30);
            }
            else if (weather & B_WEATHER_RAIN_PRIMAL)
            {
                if (moveType == TYPE_FIRE)
                    RETURN_SCORE_MINUS(30);
            }
        }
    }

    // DebugPrintf("Check Move Effects");
    // check move effects
    switch (moveEffect)
    {
        case EFFECT_HIT: 
            // only applies to Vital Throw
            if (GetBattleMovePriority(battlerAtk, move) < 0 && AI_IsFaster(battlerAtk, battlerDef, move) && aiData->hpPercents[battlerAtk] < 40)
                RETURN_SCORE_MINUS(10);    // don't want to move last
            break;
        default:
            break;  // check move damage
        case EFFECT_SLEEP:
            // DebugPrintf("CASE SLEEP");
            if (!AI_CanPutToSleep(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, aiData->partnerMove))
                RETURN_SCORE_MINUS(10);
            if (PartnerMoveActivatesSleepClause(aiData->partnerMove))
                RETURN_SCORE_MINUS(20);
            break;
        case EFFECT_EXPLOSION:
            if (AI_IsFaster(battlerAtk, battlerDef, move) && aiData->hpPercents[battlerAtk] > 40)
                ADJUST_SCORE(-10);
            if (effectiveness == UQ_4_12(0.0))
            {
                ADJUST_SCORE(-30);
            }
            else if (IsAbilityOnField(ABILITY_DAMP) && !DoesBattlerIgnoreAbilityChecks(battlerAtk, aiData->abilities[battlerAtk], move))
            {
                ADJUST_SCORE(-30);
            }
            else if (CountUsablePartyMons(battlerAtk) == 0)
            {
                if (CountUsablePartyMons(battlerDef) != 0)
                    ADJUST_SCORE(-10);
            }
            break;
    // stat raising effects
        case EFFECT_ATTACK_UP:
        case EFFECT_ATTACK_UP_2:
        case EFFECT_ATTACK_UP_USER_ALLY:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_STUFF_CHEEKS:
            if (ItemId_GetPocket(gBattleMons[battlerAtk].item) != POCKET_BERRIES)
                return 0;   // cannot even select
            //fallthrough
        case EFFECT_DEFENSE_UP:
        case EFFECT_DEFENSE_UP_2:
        case EFFECT_DEFENSE_UP_3:
        case EFFECT_DEFENSE_CURL:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_DEF))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SPECIAL_ATTACK_UP:
        case EFFECT_SPECIAL_ATTACK_UP_2:
        case EFFECT_SPECIAL_ATTACK_UP_3:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_SPECIAL))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SPECIAL_DEFENSE_UP:
        case EFFECT_SPECIAL_DEFENSE_UP_2:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPDEF))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_ACCURACY_UP:
        case EFFECT_ACCURACY_UP_2:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ACC))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_EVASION_UP:
        case EFFECT_EVASION_UP_2:
        case EFFECT_MINIMIZE:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_EVASION))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_COSMIC_POWER:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_DEF))
                ADJUST_SCORE(-10);
            else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPDEF))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_BULK_UP:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
                ADJUST_SCORE(-10);
            else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_DEF))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_CALM_MIND:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPATK))
                ADJUST_SCORE(-10);
            else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPDEF))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_DRAGON_DANCE:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
                ADJUST_SCORE(-10);
            else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPEED))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_COIL:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ACC))
                ADJUST_SCORE(-10);
            else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
                ADJUST_SCORE(-8);
            else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_DEF))
                ADJUST_SCORE(-6);
            break;
        case EFFECT_ATTACK_ACCURACY_UP: //hone claws
            if (aiData->abilities[battlerAtk] != ABILITY_CONTRARY)
            {
                if (gBattleMons[battlerAtk].statStages[STAT_ATK] >= MAX_STAT_STAGE
                  && (gBattleMons[battlerAtk].statStages[STAT_ACC] >= MAX_STAT_STAGE || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL)))
                    ADJUST_SCORE(-10);
                break;
            }
            else
            {
                ADJUST_SCORE(-10);
            }
            break;
        case EFFECT_CHARGE:
            if (gStatuses3[battlerAtk] & STATUS3_CHARGED_UP)
                ADJUST_SCORE(-20);
            else if (!HasMoveWithType(battlerAtk, TYPE_ELECTRIC))
                ADJUST_SCORE(-10);
            else if (B_CHARGE_SPDEF_RAISE >= GEN_5
              && !BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPDEF))
                ADJUST_SCORE(-5);
            break;
        case EFFECT_QUIVER_DANCE:
        case EFFECT_GEOMANCY:
            if (gBattleMons[battlerAtk].statStages[STAT_SPATK] >= MAX_STAT_STAGE || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_SPECIAL))
                ADJUST_SCORE(-10);
            else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPEED))
                ADJUST_SCORE(-8);
            else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPDEF))
                ADJUST_SCORE(-6);
            break;
        case EFFECT_VICTORY_DANCE:
            if (gBattleMons[battlerAtk].statStages[STAT_ATK] >= MAX_STAT_STAGE || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
                ADJUST_SCORE(-10);
            else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPEED))
                ADJUST_SCORE(-8);
            else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_DEF))
                ADJUST_SCORE(-6);
            break;
        case EFFECT_SHIFT_GEAR:
            if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
                ADJUST_SCORE(-10);
            else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPEED))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_SHELL_SMASH:
            if (aiData->abilities[battlerAtk] == ABILITY_CONTRARY)
            {
                if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_DEF))
                    ADJUST_SCORE(-10);
                else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPDEF))
                    ADJUST_SCORE(-8);
            }
            else
            {
                if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
                    ADJUST_SCORE(-10);
                else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_SPECIAL))
                    ADJUST_SCORE(-8);
                else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPEED))
                    ADJUST_SCORE(-6);
            }
            break;
        case EFFECT_GROWTH:
        case EFFECT_ATTACK_SPATK_UP:    // work up
            if ((!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ATK) && !BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPATK))
             || (!HasDamagingMove(battlerAtk)))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_ROTOTILLER:
            if (isDoubleBattle)
            {
                if (!(IS_BATTLER_OF_TYPE(battlerAtk, TYPE_GRASS)
                  && IsBattlerGrounded(battlerAtk)
                  && (BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ATK) || BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPATK)))
                  && !(IS_BATTLER_OF_TYPE(BATTLE_PARTNER(battlerAtk), TYPE_GRASS)
                  && IsBattlerGrounded(BATTLE_PARTNER(battlerAtk))
                  && aiData->abilities[BATTLE_PARTNER(battlerAtk)] != ABILITY_CONTRARY
                  && (BattlerStatCanRise(BATTLE_PARTNER(battlerAtk), aiData->abilities[BATTLE_PARTNER(battlerAtk)], STAT_ATK)
                   || BattlerStatCanRise(BATTLE_PARTNER(battlerAtk), aiData->abilities[BATTLE_PARTNER(battlerAtk)], STAT_SPATK))))
                {
                    ADJUST_SCORE(-10);
                }
            }
            else if (!(IS_BATTLER_OF_TYPE(battlerAtk, TYPE_GRASS)
              && IsBattlerGrounded(battlerAtk)
              && (BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ATK) || BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPATK))))
            {
                ADJUST_SCORE(-10);
            }
            break;
        case EFFECT_GEAR_UP:
            if (aiData->abilities[battlerAtk] == ABILITY_PLUS || aiData->abilities[battlerAtk] == ABILITY_MINUS)
            {
                // same as growth, work up
                if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
                    ADJUST_SCORE(-10);
                else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_SPECIAL))
                    ADJUST_SCORE(-8);
                break;
            }
            else if (!isDoubleBattle)
            {
                ADJUST_SCORE(-10);    // no partner and our stats wont rise, so don't use
            }

            if (isDoubleBattle)
            {
                if (aiData->abilities[BATTLE_PARTNER(battlerAtk)] == ABILITY_PLUS || aiData->abilities[BATTLE_PARTNER(battlerAtk)] == ABILITY_MINUS)
                {
                    if ((!BattlerStatCanRise(BATTLE_PARTNER(battlerAtk), aiData->abilities[BATTLE_PARTNER(battlerAtk)], STAT_ATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
                      && (!BattlerStatCanRise(BATTLE_PARTNER(battlerAtk), aiData->abilities[BATTLE_PARTNER(battlerAtk)], STAT_SPATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_SPECIAL)))
                        ADJUST_SCORE(-10);
                }
                else if (aiData->abilities[battlerAtk] != ABILITY_PLUS && aiData->abilities[battlerAtk] != ABILITY_MINUS)
                {
                    ADJUST_SCORE(-10);    // nor our or our partner's ability is plus/minus
                }
            }
            break;
        case EFFECT_ACUPRESSURE:
            if (DoesSubstituteBlockMove(battlerAtk, battlerDef, move) || AreBattlersStatsMaxed(battlerDef))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_MAGNETIC_FLUX:
            if (aiData->abilities[battlerAtk] == ABILITY_PLUS || aiData->abilities[battlerAtk] == ABILITY_MINUS)
            {
                if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_DEF))
                    ADJUST_SCORE(-10);
                else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPDEF))
                    ADJUST_SCORE(-8);
            }
            else if (!isDoubleBattle)
            {
                ADJUST_SCORE(-10);    // our stats wont rise from this move
            }

            if (isDoubleBattle)
            {
                if (aiData->abilities[BATTLE_PARTNER(battlerAtk)] == ABILITY_PLUS || aiData->abilities[BATTLE_PARTNER(battlerAtk)] == ABILITY_MINUS)
                {
                    if (!BattlerStatCanRise(BATTLE_PARTNER(battlerAtk), aiData->abilities[BATTLE_PARTNER(battlerAtk)], STAT_DEF))
                        ADJUST_SCORE(-10);
                    else if (!BattlerStatCanRise(BATTLE_PARTNER(battlerAtk), aiData->abilities[BATTLE_PARTNER(battlerAtk)], STAT_SPDEF))
                        ADJUST_SCORE(-8);
                }
                else if (aiData->abilities[battlerAtk] != ABILITY_PLUS && aiData->abilities[battlerAtk] != ABILITY_MINUS)
                {
                    ADJUST_SCORE(-10);    // nor our or our partner's ability is plus/minus
                }
            }
            break;
    // stat lowering effects
        case EFFECT_ATTACK_DOWN:
        case EFFECT_ATTACK_DOWN_2:
            if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_ATK)) //|| !HasMoveWithCategory(battlerDef, DAMAGE_CATEGORY_PHYSICAL))
                ADJUST_SCORE(-10);
            else if (aiData->abilities[battlerDef] == ABILITY_HYPER_CUTTER)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_DEFENSE_DOWN:
        case EFFECT_DEFENSE_DOWN_2:
            if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_DEF))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SPEED_DOWN:
        case EFFECT_SPEED_DOWN_2:
            if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_SPEED))
                ADJUST_SCORE(-10);
            else if (aiData->abilities[battlerDef] == ABILITY_SPEED_BOOST)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SPECIAL_ATTACK_DOWN:
        case EFFECT_SPECIAL_ATTACK_DOWN_2:
            if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_SPATK)) //|| !HasMoveWithCategory(battlerDef, DAMAGE_CATEGORY_SPECIAL))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SPECIAL_DEFENSE_DOWN:
        case EFFECT_SPECIAL_DEFENSE_DOWN_2:
            if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_SPDEF))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_ACCURACY_DOWN:
        case EFFECT_ACCURACY_DOWN_2:
            if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_ACC))
                ADJUST_SCORE(-10);
            else if (aiData->abilities[battlerDef] == ABILITY_KEEN_EYE || aiData->abilities[battlerDef] == ABILITY_MINDS_EYE
                        || (B_ILLUMINATE_EFFECT >= GEN_9 && aiData->abilities[battlerDef] == ABILITY_ILLUMINATE))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_EVASION_DOWN:
        case EFFECT_EVASION_DOWN_2:
        case EFFECT_TICKLE:
            if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_ATK))
                ADJUST_SCORE(-10);
            else if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_DEF))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_VENOM_DRENCH:
            if (!(gBattleMons[battlerDef].status1 & STATUS1_PSN_ANY))
            {
                ADJUST_SCORE(-10);
            }
            else
            {
                if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_SPEED))
                    ADJUST_SCORE(-10);
                else if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_SPATK))
                    ADJUST_SCORE(-8);
                else if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_ATK))
                    ADJUST_SCORE(-6);
            }
            break;
        case EFFECT_NOBLE_ROAR:
            if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_SPATK))
                ADJUST_SCORE(-10);
            else if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_ATK))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_CAPTIVATE:
            if (!AreBattlersOfOppositeGender(battlerAtk, battlerDef))
                ADJUST_SCORE(-10);
            break;
    // other
        case EFFECT_HAZE:
            if (PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
            {
                ADJUST_SCORE(-10);    // partner already using haze
            }
            else
            {
                for (i = STAT_ATK; i < NUM_BATTLE_STATS; i++)
                {
                    if (gBattleMons[battlerAtk].statStages[i] > DEFAULT_STAT_STAGE || gBattleMons[BATTLE_PARTNER(battlerAtk)].statStages[i] > DEFAULT_STAT_STAGE)
                        ADJUST_SCORE(-10);  // Don't want to reset our boosted stats
                }
                for (i = STAT_ATK; i < NUM_BATTLE_STATS; i++)
                {
                    if (gBattleMons[battlerDef].statStages[i] < DEFAULT_STAT_STAGE || gBattleMons[BATTLE_PARTNER(battlerDef)].statStages[i] < DEFAULT_STAT_STAGE)
                        ADJUST_SCORE(-10); //Don't want to reset enemy lowered stats
                }
            }
            break;
        case EFFECT_PRESENT:
        case EFFECT_FIXED_DAMAGE_ARG:
        case EFFECT_FOCUS_PUNCH:
            // AI_CBM_HighRiskForDamage
            if (aiData->abilities[battlerDef] == ABILITY_WONDER_GUARD && effectiveness < UQ_4_12(2.0))
                ADJUST_SCORE(-10);
            if (HasDamagingMove(battlerDef) && !((gBattleMons[battlerAtk].status2 & STATUS2_SUBSTITUTE)
             || IsBattlerIncapacitated(battlerDef, aiData->abilities[battlerDef])
             || gBattleMons[battlerDef].status2 & (STATUS2_INFATUATION | STATUS2_CONFUSION)))
                ADJUST_SCORE(-10);
            if (HasMoveEffect(battlerAtk, EFFECT_SUBSTITUTE) && !(gBattleMons[battlerAtk].status2 & STATUS2_SUBSTITUTE))
                ADJUST_SCORE(-10);
            if (HasMoveEffect(battlerAtk, EFFECT_SLEEP) && ! (gBattleMons[battlerDef].status1 & STATUS1_SLEEP))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_COUNTER:
        case EFFECT_MIRROR_COAT:
            if (IsBattlerIncapacitated(battlerDef, aiData->abilities[battlerDef]) || gBattleMons[battlerDef].status2 & (STATUS2_INFATUATION | STATUS2_CONFUSION))
                ADJUST_SCORE(-1);
            if ((predictedMove == MOVE_NONE || GetBattleMoveCategory(predictedMove) == DAMAGE_CATEGORY_STATUS
              || DoesSubstituteBlockMove(battlerAtk, BATTLE_PARTNER(battlerDef), predictedMove))
              && !(predictedMove == MOVE_NONE && (AI_THINKING_STRUCT->aiFlags[battlerAtk] & AI_FLAG_RISKY))) // Let Risky AI predict blindly based on stats
                ADJUST_SCORE(-10);
            break;

        case EFFECT_ROAR:
            if (CountUsablePartyMons(battlerDef) == 0)
                ADJUST_SCORE(-10);
            else if (aiData->abilities[battlerDef] == ABILITY_SUCTION_CUPS)
                ADJUST_SCORE(-10);
            else if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_TOXIC_THREAD:
            if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_SPEED))
                ADJUST_SCORE(-1);    // may still want to just poison
            //fallthrough
        case EFFECT_POISON:
        case EFFECT_TOXIC:
            if (!AI_CanPoison(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_LIGHT_SCREEN:
            if (gSideStatuses[GetBattlerSide(battlerAtk)] & SIDE_STATUS_LIGHTSCREEN
              || PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_REFLECT:
            if (gSideStatuses[GetBattlerSide(battlerAtk)] & SIDE_STATUS_REFLECT
              || PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_AURORA_VEIL:
            if (gSideStatuses[GetBattlerSide(battlerAtk)] & SIDE_STATUS_AURORA_VEIL
              || PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove)
              || !(weather & (B_WEATHER_HAIL | B_WEATHER_SNOW)))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_OHKO:
            if (B_SHEER_COLD_IMMUNITY >= GEN_7 && move == MOVE_SHEER_COLD && IS_BATTLER_OF_TYPE(battlerDef, TYPE_ICE))
                RETURN_SCORE_MINUS(20);
            if (!ShouldTryOHKO(battlerAtk, battlerDef, aiData->abilities[battlerAtk], aiData->abilities[battlerDef], move))
                ADJUST_SCORE(-10);
            else if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_MIST:
            if (gSideStatuses[GetBattlerSide(battlerAtk)] & SIDE_STATUS_MIST
              || PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_FOCUS_ENERGY:
            if (gBattleMons[battlerAtk].status2 & STATUS2_FOCUS_ENERGY_ANY)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_CONFUSE:
        case EFFECT_SWAGGER:
        case EFFECT_FLATTER:
            if (!AI_CanConfuse(battlerAtk, battlerDef, aiData->abilities[battlerDef], BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_PARALYZE:
            if (!AI_CanParalyze(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SUBSTITUTE:
            if (gBattleMons[battlerAtk].status2 & STATUS2_SUBSTITUTE || aiData->abilities[battlerDef] == ABILITY_INFILTRATOR)
                ADJUST_SCORE(-8);
            else if (aiData->hpPercents[battlerAtk] <= 25)
                ADJUST_SCORE(-10);
            else if (HasSubstituteIgnoringMove(battlerDef))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_SHED_TAIL:
            if (CountUsablePartyMons(battlerAtk) == 0)
                ADJUST_SCORE(-10);
            if (gBattleMons[battlerAtk].status2 & STATUS2_SUBSTITUTE || aiData->abilities[battlerDef] == ABILITY_INFILTRATOR)
                ADJUST_SCORE(-8);
            else if (aiData->hpPercents[battlerAtk] <= 50)
                ADJUST_SCORE(-10);
            else if (HasSubstituteIgnoringMove(battlerDef))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_LEECH_SEED:
            if (gStatuses3[battlerDef] & STATUS3_LEECHSEED
             || IS_BATTLER_OF_TYPE(battlerDef, TYPE_GRASS)
             || DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            else if (aiData->abilities[battlerDef] == ABILITY_LIQUID_OOZE)
                ADJUST_SCORE(-3);
            break;
        case EFFECT_DISABLE:
            if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
                ADJUST_SCORE(-10);
            else if (gDisableStructs[battlerDef].disableTimer == 0
                && (B_MENTAL_HERB < GEN_5 || aiData->holdEffects[battlerDef] != HOLD_EFFECT_MENTAL_HERB)
                && !PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
            {
                if (AI_IsFaster(battlerAtk, battlerDef, move)) // Attacker should go first
                {
                    if (gLastMoves[battlerDef] == MOVE_NONE || gLastMoves[battlerDef] == 0xFFFF)
                        ADJUST_SCORE(-10);    // no anticipated move to disable
                }
                else if (predictedMove == MOVE_NONE)
                {
                    ADJUST_SCORE(-10);
                }
            }
            else
            {
                ADJUST_SCORE(-10);
            }
            break;
        case EFFECT_ENCORE:
            if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
                ADJUST_SCORE(-10);
            else if (gDisableStructs[battlerDef].encoreTimer == 0
                && (B_MENTAL_HERB < GEN_5 || aiData->holdEffects[battlerDef] != HOLD_EFFECT_MENTAL_HERB)
                && !DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
            {
                if (AI_IsFaster(battlerAtk, battlerDef, move)) // Attacker should go first
                {
                    if (gLastMoves[battlerDef] == MOVE_NONE || gLastMoves[battlerDef] == 0xFFFF)
                        ADJUST_SCORE(-10);    // no anticipated move to encore
                }
                else if (predictedMove == MOVE_NONE)
                {
                    ADJUST_SCORE(-10);
                }
            }
            else
            {
                ADJUST_SCORE(-10);
            }
            break;
        case EFFECT_SNORE:
        case EFFECT_SLEEP_TALK:
            if (IsWakeupTurn(battlerAtk) || !AI_IsBattlerAsleepOrComatose(battlerAtk))
                ADJUST_SCORE(-10);    // if mon will wake up, is not asleep, or is not comatose
            break;
        case EFFECT_MEAN_LOOK:
            if (IsBattlerTrapped(battlerDef, TRUE) || DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_NIGHTMARE:
            if (gBattleMons[battlerDef].status2 & STATUS2_NIGHTMARE)
                ADJUST_SCORE(-10);
            else if (!AI_IsBattlerAsleepOrComatose(battlerDef))
                ADJUST_SCORE(-8);
            else if (DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_CURSE:
            if (IS_BATTLER_OF_TYPE(battlerAtk, TYPE_GHOST))
            {
                if (gBattleMons[battlerDef].status2 & STATUS2_CURSED
                  || DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                    ADJUST_SCORE(-10);
                else if (aiData->hpPercents[battlerAtk] <= 50)
                    ADJUST_SCORE(-6);
            }
            else // regular curse
            {
                if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_ATK) || !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
                    ADJUST_SCORE(-10);
                else if (!BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_DEF))
                    ADJUST_SCORE(-8);
            }
            break;
        case EFFECT_SPIKES:
            if (gSideTimers[GetBattlerSide(battlerDef)].spikesAmount >= 3)
                ADJUST_SCORE(-10);
            else if (PartnerMoveIsSameNoTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove)
              && gSideTimers[GetBattlerSide(battlerDef)].spikesAmount == 2)
                ADJUST_SCORE(-10); // only one mon needs to set up the last layer of Spikes
            break;
        case EFFECT_STEALTH_ROCK:
            if (gSideTimers[GetBattlerSide(battlerDef)].stealthRockAmount > 0
              || PartnerMoveIsSameNoTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove)) //Only one mon needs to set up Stealth Rocks
                ADJUST_SCORE(-10);
            break;
        case EFFECT_TOXIC_SPIKES:
            if (gSideTimers[GetBattlerSide(battlerDef)].toxicSpikesAmount >= 2)
                ADJUST_SCORE(-10);
            else if (PartnerMoveIsSameNoTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove) && gSideTimers[GetBattlerSide(battlerDef)].toxicSpikesAmount == 1)
                ADJUST_SCORE(-10); // only one mon needs to set up the last layer of Toxic Spikes
            break;
        case EFFECT_STICKY_WEB:
            if (gSideTimers[GetBattlerSide(battlerDef)].stickyWebAmount)
                ADJUST_SCORE(-10);
            else if (PartnerMoveIsSameNoTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove) && gSideTimers[GetBattlerSide(battlerDef)].stickyWebAmount)
                ADJUST_SCORE(-10); // only one mon needs to set up Sticky Web
            break;
        case EFFECT_FORESIGHT:
            if (gBattleMons[battlerDef].status2 & STATUS2_FORESIGHT)
                ADJUST_SCORE(-10);
            else if (gBattleMons[battlerDef].statStages[STAT_EVASION] <= 4
              || !(IS_BATTLER_OF_TYPE(battlerDef, TYPE_GHOST))
              || DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-9);
            break;
        case EFFECT_PERISH_SONG:
            if (isDoubleBattle)
            {
                if (CountUsablePartyMons(battlerAtk) == 0
                  && aiData->abilities[battlerAtk] != ABILITY_SOUNDPROOF
                  && aiData->abilities[BATTLE_PARTNER(battlerAtk)] != ABILITY_SOUNDPROOF
                  && CountUsablePartyMons(FOE(battlerAtk)) >= 1)
                {
                    ADJUST_SCORE(-10); //Don't wipe your team if you're going to lose
                }
                else if ((!IsBattlerAlive(FOE(battlerAtk)) || aiData->abilities[FOE(battlerAtk)] == ABILITY_SOUNDPROOF
                  || gStatuses3[FOE(battlerAtk)] & STATUS3_PERISH_SONG)
                  && (!IsBattlerAlive(BATTLE_PARTNER(FOE(battlerAtk))) || aiData->abilities[BATTLE_PARTNER(FOE(battlerAtk))] == ABILITY_SOUNDPROOF
                  || gStatuses3[BATTLE_PARTNER(FOE(battlerAtk))] & STATUS3_PERISH_SONG))
                {
                    ADJUST_SCORE(-10); //Both enemies are perish songed
                }
                else if (DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                {
                    ADJUST_SCORE(-10);
                }
            }
            else
            {
                if (CountUsablePartyMons(battlerAtk) == 0 && aiData->abilities[battlerAtk] != ABILITY_SOUNDPROOF
                  && CountUsablePartyMons(battlerDef) >= 1)
                    ADJUST_SCORE(-10);

                if (gStatuses3[FOE(battlerAtk)] & STATUS3_PERISH_SONG || aiData->abilities[FOE(battlerAtk)] == ABILITY_SOUNDPROOF)
                    ADJUST_SCORE(-10);
            }
            break;
        case EFFECT_SANDSTORM:
            if (weather & (B_WEATHER_SANDSTORM | B_WEATHER_PRIMAL_ANY)
             || IsMoveEffectWeather(aiData->partnerMove))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_SUNNY_DAY:
            if (weather & (B_WEATHER_SUN | B_WEATHER_PRIMAL_ANY)
             || IsMoveEffectWeather(aiData->partnerMove))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_RAIN_DANCE:
            if (weather & (B_WEATHER_RAIN | B_WEATHER_PRIMAL_ANY)
             || IsMoveEffectWeather(aiData->partnerMove))
                ADJUST_SCORE(-8);
            break;
        case EFFECT_HAIL:
            if (weather & (B_WEATHER_HAIL | B_WEATHER_PRIMAL_ANY)
             || IsMoveEffectWeather(aiData->partnerMove))
                ADJUST_SCORE(-8);
            else if (weather & B_WEATHER_SNOW)
                ADJUST_SCORE(-2); // mainly to prevent looping between hail and snow
            break;
        case EFFECT_SNOWSCAPE:
            if (weather & (B_WEATHER_SNOW | B_WEATHER_PRIMAL_ANY)
             || IsMoveEffectWeather(aiData->partnerMove))
                ADJUST_SCORE(-8);
            else if (weather & B_WEATHER_HAIL)
                ADJUST_SCORE(-2); // mainly to prevent looping between hail and snow
            break;
        case EFFECT_ATTRACT:
            if (!AI_CanBeInfatuated(battlerAtk, battlerDef, aiData->abilities[battlerDef]))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SAFEGUARD:
            if (gSideStatuses[GetBattlerSide(battlerAtk)] & SIDE_STATUS_SAFEGUARD
              || PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_MAGNITUDE:
            if (aiData->abilities[battlerDef] == ABILITY_LEVITATE)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_PARTING_SHOT:
            if (CountUsablePartyMons(battlerAtk) == 0)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_BATON_PASS:
            if (CountUsablePartyMons(battlerAtk) == 0)
                ADJUST_SCORE(-10);
            else if (gBattleMons[battlerAtk].status2 & STATUS2_SUBSTITUTE
              || (gStatuses3[battlerAtk] & (STATUS3_ROOTED | STATUS3_AQUA_RING | STATUS3_MAGNET_RISE | STATUS3_POWER_TRICK))
              || AnyStatIsRaised(battlerAtk))
                break;
            else
                ADJUST_SCORE(-6);
            break;
        case EFFECT_HIT_ESCAPE:
            break;
        case EFFECT_CHILLY_RECEPTION:
            if (CountUsablePartyMons(battlerAtk) == 0)
                ADJUST_SCORE(-10);
            else if (weather & (B_WEATHER_SNOW | B_WEATHER_PRIMAL_ANY) || IsMoveEffectWeather(aiData->partnerMove))
                ADJUST_SCORE(-8);
            else if (weather & B_WEATHER_HAIL)
                ADJUST_SCORE(-2); // mainly to prevent looping between hail and snow
            break;
        case EFFECT_BELLY_DRUM:
        case EFFECT_FILLET_AWAY:
            if (aiData->abilities[battlerAtk] == ABILITY_CONTRARY)
                ADJUST_SCORE(-10);
            else if (aiData->hpPercents[battlerAtk] <= 60)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_FUTURE_SIGHT:
            if (gSideStatuses[GetBattlerSide(battlerDef)] & SIDE_STATUS_FUTUREATTACK
              || gSideStatuses[GetBattlerSide(battlerAtk)] & SIDE_STATUS_FUTUREATTACK)
                ADJUST_SCORE(-12);
            else
                ADJUST_SCORE(GOOD_EFFECT);
            break;
        case EFFECT_TELEPORT:
            ADJUST_SCORE(-10);
            break;
        case EFFECT_FIRST_TURN_ONLY:
            if (!gDisableStructs[battlerAtk].isFirstTurn)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_STOCKPILE:
            if (gDisableStructs[battlerAtk].stockpileCounter >= 3)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SWALLOW:
            if (gDisableStructs[battlerAtk].stockpileCounter == 0)
            {
                ADJUST_SCORE(-10);
            }
            else
            {
                if (AI_BattlerAtMaxHp(battlerAtk))
                    ADJUST_SCORE(-10);
                else if (aiData->hpPercents[battlerAtk] >= 80)
                    ADJUST_SCORE(-5); // do it if nothing better
            }
            break;
        case EFFECT_TORMENT:
            if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
                ADJUST_SCORE(-10);
            else if (gBattleMons[battlerDef].status2 & STATUS2_TORMENT
              || DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
            {
                ADJUST_SCORE(-10);
                break;
            }
            if (B_MENTAL_HERB >= GEN_5 && aiData->holdEffects[battlerDef] == HOLD_EFFECT_MENTAL_HERB)
                ADJUST_SCORE(-6);
            break;
        case EFFECT_WILL_O_WISP:
            if (!AI_CanBurn(battlerAtk, battlerDef, aiData->abilities[battlerDef], BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_MEMENTO:
            if (CountUsablePartyMons(battlerAtk) == 0 || DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            else if (gBattleMons[battlerDef].statStages[STAT_ATK] == MIN_STAT_STAGE && gBattleMons[battlerDef].statStages[STAT_SPATK] == MIN_STAT_STAGE)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_FOLLOW_ME:
        case EFFECT_HELPING_HAND:
            if (!isDoubleBattle
              || !IsBattlerAlive(BATTLE_PARTNER(battlerAtk))
              || PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove)
              || (aiData->partnerMove != MOVE_NONE && IsBattleMoveStatus(aiData->partnerMove))
              || gBattleStruct->monToSwitchIntoId[BATTLE_PARTNER(battlerAtk)] != PARTY_SIZE) //Partner is switching out.
                ADJUST_SCORE(-10);
            break;
        case EFFECT_TRICK:
        case EFFECT_KNOCK_OFF:
        case EFFECT_CORROSIVE_GAS:
            if (aiData->abilities[battlerDef] == ABILITY_STICKY_HOLD)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_INGRAIN:
            if (gStatuses3[battlerAtk] & STATUS3_ROOTED)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_AQUA_RING:
            if (gStatuses3[battlerAtk] & STATUS3_AQUA_RING)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_RECYCLE:
            if (GetUsedHeldItem(battlerAtk) == 0 || gBattleMons[battlerAtk].item != 0)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_IMPRISON:
            if (gStatuses3[battlerAtk] & STATUS3_IMPRISONED_OTHERS)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_REFRESH:
            if (!(gBattleMons[battlerDef].status1 & (STATUS1_PSN_ANY | STATUS1_BURN | STATUS1_PARALYSIS | STATUS1_FROSTBITE)))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_PSYCHO_SHIFT:
            if (gBattleMons[battlerAtk].status1 & STATUS1_PSN_ANY && !AI_CanPoison(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            else if (gBattleMons[battlerAtk].status1 & STATUS1_BURN && !AI_CanBurn(battlerAtk, battlerDef,
              aiData->abilities[battlerDef], BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            else if (gBattleMons[battlerAtk].status1 & STATUS1_FROSTBITE && !AI_CanGiveFrostbite(battlerAtk, battlerDef,
              aiData->abilities[battlerDef], BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            else if (gBattleMons[battlerAtk].status1 & STATUS1_PARALYSIS && !AI_CanParalyze(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            else if (gBattleMons[battlerAtk].status1 & STATUS1_SLEEP && !AI_CanPutToSleep(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            else
                ADJUST_SCORE(-10);    // attacker has no status to transmit
            break;
        case EFFECT_MUD_SPORT:
            if (gFieldStatuses & STATUS_FIELD_MUDSPORT
              || gStatuses4[battlerAtk] & STATUS4_MUD_SPORT
              || PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_WATER_SPORT:
            if (gFieldStatuses & STATUS_FIELD_WATERSPORT
              || gStatuses4[battlerAtk] & STATUS4_WATER_SPORT
              || PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_ABSORB:
            if (aiData->abilities[battlerDef] == ABILITY_LIQUID_OOZE)
                ADJUST_SCORE(-6);
            break;
        case EFFECT_STRENGTH_SAP:
            if (aiData->abilities[battlerDef] == ABILITY_CONTRARY)
                ADJUST_SCORE(-10);
            else if (!ShouldLowerStat(battlerAtk, battlerDef, aiData->abilities[battlerDef], STAT_ATK))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_COPYCAT:
        case EFFECT_MIRROR_MOVE:
            return AI_CheckBadMove(battlerAtk, battlerDef, predictedMove, score);
        case EFFECT_FLOWER_SHIELD:
            if (!IS_BATTLER_OF_TYPE(battlerAtk, TYPE_GRASS)
              && !(isDoubleBattle && IS_BATTLER_OF_TYPE(BATTLE_PARTNER(battlerAtk), TYPE_GRASS)))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_AROMATIC_MIST:
            if (!isDoubleBattle || gBattleMons[BATTLE_PARTNER(battlerAtk)].hp == 0 || !BattlerStatCanRise(BATTLE_PARTNER(battlerAtk), aiData->abilities[BATTLE_PARTNER(battlerAtk)], STAT_SPDEF))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_BIDE:
            if (!HasDamagingMove(battlerDef)
              || aiData->hpPercents[battlerAtk] < 30 //Close to death
              || gBattleMons[battlerDef].status1 & (STATUS1_SLEEP | STATUS1_FREEZE)) //No point in biding if can't take damage
                ADJUST_SCORE(-10);
            break;
        case EFFECT_HIT_SWITCH_TARGET:
            if (DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10); // don't scare away pokemon twice
            else if (aiData->hpPercents[battlerDef] < 10 && GetBattlerSecondaryDamage(battlerDef))
                ADJUST_SCORE(-10);    // don't blow away mon that will faint soon
            else if (gStatuses3[battlerDef] & STATUS3_PERISH_SONG)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_CONVERSION:
            //Check first move type
            if (IS_BATTLER_OF_TYPE(battlerAtk, GetMoveType(gBattleMons[battlerAtk].moves[0])))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_REST:
            if (!CanBeSlept(battlerAtk, aiData->abilities[battlerAtk], NOT_BLOCKED_BY_SLEEP_CLAUSE))
                ADJUST_SCORE(-10);
            //fallthrough
        case EFFECT_RESTORE_HP:
        case EFFECT_SOFTBOILED:
        case EFFECT_ROOST:
            if (AI_BattlerAtMaxHp(battlerAtk))
                ADJUST_SCORE(-10);
            else if (aiData->hpPercents[battlerAtk] >= 90)
                ADJUST_SCORE(-9); //No point in healing, but should at least do it if nothing better
            break;
        case EFFECT_MORNING_SUN:
        case EFFECT_SYNTHESIS:
        case EFFECT_MOONLIGHT:
            if ((AI_GetWeather() & (B_WEATHER_RAIN | B_WEATHER_SANDSTORM | B_WEATHER_HAIL | B_WEATHER_SNOW | B_WEATHER_FOG)))
                ADJUST_SCORE(-3);
            else if (AI_BattlerAtMaxHp(battlerAtk))
                ADJUST_SCORE(-10);
            else if (aiData->hpPercents[battlerAtk] >= 90)
                ADJUST_SCORE(-9); //No point in healing, but should at least do it if nothing better
            break;
        case EFFECT_PURIFY:
            if (!(gBattleMons[battlerDef].status1 & STATUS1_ANY))
                ADJUST_SCORE(-10);
            else if (battlerDef == BATTLE_PARTNER(battlerAtk))
                break; //Always heal your ally
            else if (AI_BattlerAtMaxHp(battlerAtk))
                ADJUST_SCORE(-10);
            else if (aiData->hpPercents[battlerAtk] >= 90)
                ADJUST_SCORE(-8); //No point in healing, but should at least do it if nothing better
            break;
        case EFFECT_RECOIL_IF_MISS:
            if (aiData->abilities[battlerAtk] != ABILITY_MAGIC_GUARD && AI_DATA->moveAccuracy[battlerAtk][battlerDef][AI_THINKING_STRUCT->movesetIndex] < 75
            && !(AI_THINKING_STRUCT->aiFlags[battlerAtk] & AI_FLAG_RISKY))
                ADJUST_SCORE(-6);
            break;
        case EFFECT_TRANSFORM:
            if (gBattleMons[battlerAtk].status2 & STATUS2_TRANSFORMED
              || (gBattleMons[battlerDef].status2 & (STATUS2_TRANSFORMED | STATUS2_SUBSTITUTE))) //Leave out Illusion b/c AI is supposed to be fooled
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SPITE:
        case EFFECT_MIMIC:
            if (AI_IsFaster(battlerAtk, battlerDef, move)) // Attacker should go first
            {
                if (gLastMoves[battlerDef] == MOVE_NONE
                  || gLastMoves[battlerDef] == 0xFFFF)
                    ADJUST_SCORE(-10);
            }
            else if (predictedMove == MOVE_NONE)
            {
                // TODO predicted move separate from gLastMoves
                ADJUST_SCORE(-10);
            }
            break;
        case EFFECT_METRONOME:
            break;

        case EFFECT_CONVERSION_2:
            //TODO
            break;
        case EFFECT_LOCK_ON:
            if (gStatuses3[battlerDef] & STATUS3_ALWAYS_HITS
              || aiData->abilities[battlerAtk] == ABILITY_NO_GUARD
              || aiData->abilities[battlerDef] == ABILITY_NO_GUARD
              || DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_LASER_FOCUS:
            if (gStatuses3[battlerAtk] & STATUS3_LASER_FOCUS)
                ADJUST_SCORE(-10);
            else if (aiData->abilities[battlerDef] == ABILITY_SHELL_ARMOR || aiData->abilities[battlerDef] == ABILITY_BATTLE_ARMOR)
                ADJUST_SCORE(-8);
            break;
        case EFFECT_SKETCH:
            if (gLastMoves[battlerDef] == MOVE_NONE)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_DESTINY_BOND:
            if (DoesDestinyBondFail(battlerAtk))
                ADJUST_SCORE(-10);
            if (gBattleMons[battlerDef].status2 & STATUS2_DESTINY_BOND)
                ADJUST_SCORE(-10);
            else if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_HEAL_BELL:
            if (!AnyPartyMemberStatused(battlerAtk, IsSoundMove(move)) || PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_ENDURE:
            if (gBattleMons[battlerAtk].hp == 1 || GetBattlerSecondaryDamage(battlerAtk)) //Don't use Endure if you'll die after using it
                ADJUST_SCORE(-10);
            break;
        case EFFECT_PROTECT:
            {
                bool32 decreased = FALSE;
                switch (move)
                {
                case MOVE_QUICK_GUARD:
                case MOVE_WIDE_GUARD:
                case MOVE_CRAFTY_SHIELD:
                    if (!isDoubleBattle)
                    {
                        ADJUST_SCORE(-10);
                        decreased = TRUE;
                    }
                    break;
                case MOVE_MAT_BLOCK:
                    if (!gDisableStructs[battlerAtk].isFirstTurn)
                    {
                        ADJUST_SCORE(-10);
                        decreased = TRUE;
                    }
                    break;
                } // move check

                if (decreased)
                    break;
                if (IsBattlerIncapacitated(battlerDef, aiData->abilities[battlerDef]))
                {
                    ADJUST_SCORE(-10);
                    break;
                }

                if (move != MOVE_QUICK_GUARD
                  && move != MOVE_WIDE_GUARD
                  && move != MOVE_CRAFTY_SHIELD) //These moves have infinite usage
                {
                    if (GetBattlerSecondaryDamage(battlerAtk) >= gBattleMons[battlerAtk].hp
                      && aiData->abilities[battlerDef] != ABILITY_MOXIE
                      && aiData->abilities[battlerDef] != ABILITY_BEAST_BOOST)
                    {
                        ADJUST_SCORE(-10); //Don't protect if you're going to faint after protecting
                    }
                    else if (gDisableStructs[battlerAtk].protectUses == 1 && Random() % 100 < 50)
                    {
                        if (!isDoubleBattle)
                            ADJUST_SCORE(-6);
                        else
                            ADJUST_SCORE(-10); //Don't try double protecting in doubles
                    }
                    else if (gDisableStructs[battlerAtk].protectUses >= 2)
                    {
                        ADJUST_SCORE(-10);
                    }
                }

                /*if (AI_THINKING_STRUCT->aiFlags[battlerAtk] == AI_SCRIPT_CHECK_BAD_MOVE //Only basic AI
                && IsDoubleBattle()) //Make the regular AI know how to use Protect minimally in Doubles
                {
                    u8 shouldProtect = ShouldProtect(battlerAtk, battlerDef, move);
                    if (shouldProtect == USE_PROTECT || shouldProtect == PROTECT_FROM_FOES)
                        IncreaseFoeProtectionViability(&viability, 0xFF, battlerAtk, battlerDef);
                    else if (shouldProtect == PROTECT_FROM_ALLIES)
                        IncreaseAllyProtectionViability(&viability, 0xFF);
                }*/
            }
            break;
        case EFFECT_MIRACLE_EYE:
            if (gStatuses3[battlerDef] & STATUS3_MIRACLE_EYED)
                ADJUST_SCORE(-10);

            if (gBattleMons[battlerDef].statStages[STAT_EVASION] <= 4
              || !(IS_BATTLER_OF_TYPE(battlerDef, TYPE_DARK))
              || DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-9);
            break;
        case EFFECT_DEFOG:
            if (gSideStatuses[GetBattlerSide(battlerDef)]
             & (SIDE_STATUS_SCREEN_ANY | SIDE_STATUS_SAFEGUARD | SIDE_STATUS_MIST)
              || gSideStatuses[GetBattlerSide(battlerAtk)] & SIDE_STATUS_HAZARDS_ANY)
            {
                if (PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                {
                    ADJUST_SCORE(-10); //Only need one hazards removal
                    break;
                }
            }

            if (gSideStatuses[GetBattlerSide(battlerDef)] & SIDE_STATUS_HAZARDS_ANY)
            {
                ADJUST_SCORE(-10); //Don't blow away opposing hazards
                break;
            }

            if (isDoubleBattle)
            {
                if (IsHazardMove(aiData->partnerMove) // partner is going to set up hazards
                  && AI_IsFaster(BATTLE_PARTNER(battlerAtk), battlerAtk, aiData->partnerMove)) // partner is going to set up before the potential Defog
                {
                    ADJUST_SCORE(-10);
                    break; // Don't use Defog if partner is going to set up hazards
                }
            }

            // evasion check
            if (gBattleMons[battlerDef].statStages[STAT_EVASION] == MIN_STAT_STAGE
              || ((aiData->abilities[battlerDef] == ABILITY_CONTRARY) && !IS_TARGETING_PARTNER(battlerAtk, battlerDef))) // don't want to raise target stats unless its your partner
                ADJUST_SCORE(-10);
            break;
        case EFFECT_PSYCH_UP:   // haze stats check
            {
                for (i = STAT_ATK; i < NUM_BATTLE_STATS; i++)
                {
                    if (gBattleMons[battlerAtk].statStages[i] > DEFAULT_STAT_STAGE || gBattleMons[BATTLE_PARTNER(battlerAtk)].statStages[i] > DEFAULT_STAT_STAGE)
                        ADJUST_SCORE(-10);  // Don't want to reset our boosted stats
                }
                for (i = STAT_ATK; i < NUM_BATTLE_STATS; i++)
                {
                    if (gBattleMons[battlerDef].statStages[i] < DEFAULT_STAT_STAGE || gBattleMons[BATTLE_PARTNER(battlerDef)].statStages[i] < DEFAULT_STAT_STAGE)
                        ADJUST_SCORE(-10); //Don't want to copy enemy lowered stats
                }
            }
            break;
        case EFFECT_SEMI_INVULNERABLE:
            if (predictedMove != MOVE_NONE
              && AI_IsSlower(battlerAtk, battlerDef, move)
              && GetMoveEffect(predictedMove) == EFFECT_SEMI_INVULNERABLE)
                ADJUST_SCORE(-10); // Don't Fly/dig/etc if opponent is going to fly/dig/etc after you

            if (BattlerWillFaintFromWeather(battlerAtk, aiData->abilities[battlerAtk])
              && GetMoveTwoTurnAttackStatus(move) == STATUS3_ON_AIR)
                ADJUST_SCORE(-10); // Attacker will faint while in the air
            break;
        case EFFECT_HEALING_WISH:   //healing wish, lunar dance
            if (CountUsablePartyMons(battlerAtk) == 0 || DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            else if (IsPartyFullyHealedExceptBattler(battlerAtk))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_FINAL_GAMBIT:
            if (CountUsablePartyMons(battlerAtk) == 0 || DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_NATURE_POWER:
            return AI_CheckBadMove(battlerAtk, battlerDef, GetNaturePowerMove(battlerAtk), score);
        case EFFECT_TAUNT:
            if (gDisableStructs[battlerDef].tauntTimer > 0
              || DoesPartnerHaveSameMoveEffect(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_BESTOW:
            if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_NONE
              || !CanBattlerGetOrLoseItem(battlerAtk, gBattleMons[battlerAtk].item))    // AI knows its own item
                ADJUST_SCORE(-10);
            break;
        case EFFECT_ROLE_PLAY:
            if (aiData->abilities[battlerAtk] == aiData->abilities[battlerDef]
              || aiData->abilities[battlerDef] == ABILITY_NONE
              || gAbilitiesInfo[aiData->abilities[battlerAtk]].cantBeSuppressed
              || gAbilitiesInfo[aiData->abilities[battlerDef]].cantBeCopied)
                ADJUST_SCORE(-10);
            else if (IsAbilityOfRating(aiData->abilities[battlerAtk], 5))
                ADJUST_SCORE(-4);
            break;
        case EFFECT_DOODLE: // Same as Role Play, but also check if the partner's ability should be replaced
            if (aiData->abilities[battlerAtk] == aiData->abilities[battlerDef]
             || aiData->abilities[BATTLE_PARTNER(battlerAtk)] == aiData->abilities[battlerDef]
             || aiData->abilities[battlerDef] == ABILITY_NONE
             || gAbilitiesInfo[aiData->abilities[battlerAtk]].cantBeSuppressed
             || gAbilitiesInfo[aiData->abilities[BATTLE_PARTNER(battlerAtk)]].cantBeSuppressed
             || gAbilitiesInfo[aiData->abilities[battlerDef]].cantBeCopied)
                ADJUST_SCORE(-10);
            else if (IsAbilityOfRating(aiData->abilities[battlerAtk], 5) || IsAbilityOfRating(aiData->abilities[BATTLE_PARTNER(battlerAtk)], 5))
                ADJUST_SCORE(-4);
            break;
        case EFFECT_WISH:
            if (gWishFutureKnock.wishCounter[battlerAtk] > gBattleTurnCounter)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_ASSIST:
            if (CountUsablePartyMons(battlerAtk) == 0)
                ADJUST_SCORE(-10);    // no teammates to assist from
            break;
        case EFFECT_MAGIC_COAT:
            if (!HasMagicCoatAffectedMove(battlerDef))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_YAWN:
            if (gStatuses3[battlerDef] & STATUS3_YAWN)
                ADJUST_SCORE(-10);
            else if (!AI_CanPutToSleep(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            if (PartnerMoveActivatesSleepClause(aiData->partnerMove))
                ADJUST_SCORE(-20);
            break;
        case EFFECT_SKILL_SWAP:
            if (aiData->abilities[battlerAtk] == ABILITY_NONE || aiData->abilities[battlerDef] == ABILITY_NONE
              || gAbilitiesInfo[aiData->abilities[battlerAtk]].cantBeSwapped
              || gAbilitiesInfo[aiData->abilities[battlerDef]].cantBeSwapped
              || aiData->holdEffects[battlerDef] == HOLD_EFFECT_ABILITY_SHIELD)
                ADJUST_SCORE(-10);
            else if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_WORRY_SEED:
            if (aiData->abilities[battlerDef] == ABILITY_INSOMNIA
              || gAbilitiesInfo[aiData->abilities[battlerDef]].cantBeOverwritten
              || aiData->holdEffects[battlerDef] == HOLD_EFFECT_ABILITY_SHIELD)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_GASTRO_ACID:
            if (gStatuses3[battlerDef] & STATUS3_GASTRO_ACID
              || gAbilitiesInfo[aiData->abilities[battlerDef]].cantBeSuppressed)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_ENTRAINMENT:
            if (aiData->abilities[battlerAtk] == ABILITY_NONE
              || gAbilitiesInfo[aiData->abilities[battlerAtk]].cantBeCopied
              || gAbilitiesInfo[aiData->abilities[battlerDef]].cantBeOverwritten
              || aiData->holdEffects[battlerAtk] == HOLD_EFFECT_ABILITY_SHIELD)
                ADJUST_SCORE(-10);
            else if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SIMPLE_BEAM:
            if (aiData->abilities[battlerDef] == ABILITY_SIMPLE
              || gAbilitiesInfo[aiData->abilities[battlerDef]].cantBeOverwritten
              || aiData->holdEffects[battlerDef] == HOLD_EFFECT_ABILITY_SHIELD)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SNATCH:
            if (!HasSnatchAffectedMove(battlerDef)
              || PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_POWER_TRICK:
            if (IS_TARGETING_PARTNER(battlerAtk, battlerDef))
                ADJUST_SCORE(-10);
            else if (gBattleMons[battlerAtk].defense >= gBattleMons[battlerAtk].attack && !HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_POWER_SWAP: // Don't use if attacker's stat stages are higher than opponents
            if (IS_TARGETING_PARTNER(battlerAtk, battlerDef))
                ADJUST_SCORE(-10);
            else if (gBattleMons[battlerAtk].statStages[STAT_ATK] >= gBattleMons[battlerDef].statStages[STAT_ATK]
              && gBattleMons[battlerAtk].statStages[STAT_SPATK] >= gBattleMons[battlerDef].statStages[STAT_SPATK])
                ADJUST_SCORE(-10);
            break;
        case EFFECT_GUARD_SWAP: // Don't use if attacker's stat stages are higher than opponents
            if (IS_TARGETING_PARTNER(battlerAtk, battlerDef))
                ADJUST_SCORE(-10);
            else if (gBattleMons[battlerAtk].statStages[STAT_DEF] >= gBattleMons[battlerDef].statStages[STAT_DEF]
              && gBattleMons[battlerAtk].statStages[STAT_SPDEF] >= gBattleMons[battlerDef].statStages[STAT_SPDEF])
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SPEED_SWAP:
            if (IS_TARGETING_PARTNER(battlerAtk, battlerDef))
            {
                ADJUST_SCORE(-10);
            }
            else
            {
                if (gFieldStatuses & STATUS_FIELD_TRICK_ROOM && (gBattleMons[battlerAtk].speed <= gBattleMons[battlerDef].speed))
                    ADJUST_SCORE(-10);
                else if (gBattleMons[battlerAtk].speed >= gBattleMons[battlerDef].speed)
                    ADJUST_SCORE(-10);
            }
            break;
        case EFFECT_HEART_SWAP:
            if (IS_TARGETING_PARTNER(battlerAtk, battlerDef))
            {
                ADJUST_SCORE(-10);
            }
            else
            {
                u32 atkPositiveStages = CountPositiveStatStages(battlerAtk);
                u32 atkNegativeStages = CountNegativeStatStages(battlerAtk);
                u32 defPositiveStages = CountPositiveStatStages(battlerDef);
                u32 defNegativeStages = CountNegativeStatStages(battlerDef);

                if (atkPositiveStages >= defPositiveStages && atkNegativeStages <= defNegativeStages)
                    ADJUST_SCORE(-10);
                break;
            }
            break;
        case EFFECT_POWER_SPLIT:
            if (IS_TARGETING_PARTNER(battlerAtk, battlerDef))
            {
                ADJUST_SCORE(-10);
            }
            else
            {
                u32 atkAttack = gBattleMons[battlerAtk].attack;
                u32 defAttack = gBattleMons[battlerDef].attack;
                u32 atkSpAttack = gBattleMons[battlerAtk].spAttack;
                u32 defSpAttack = gBattleMons[battlerDef].spAttack;

                if (atkAttack + atkSpAttack >= defAttack + defSpAttack) // Combined attacker stats are > than combined target stats
                    ADJUST_SCORE(-10);
                break;
            }
            break;
        case EFFECT_GUARD_SPLIT:
            if (IS_TARGETING_PARTNER(battlerAtk, battlerDef))
            {
                ADJUST_SCORE(-10);
            }
            else
            {
                u32 atkDefense = gBattleMons[battlerAtk].defense;
                u32 defDefense = gBattleMons[battlerDef].defense;
                u32 atkSpDefense = gBattleMons[battlerAtk].spDefense;
                u32 defSpDefense = gBattleMons[battlerDef].spDefense;

                if (atkDefense + atkSpDefense >= defDefense + defSpDefense) //Combined attacker stats are > than combined target stats
                    ADJUST_SCORE(-10);
                break;
            }
            break;
        case EFFECT_ME_FIRST:
            if (predictedMove != MOVE_NONE)
            {
                if (AI_IsSlower(battlerAtk, battlerDef, move))
                    ADJUST_SCORE(-10);    // Target is predicted to go first, Me First will fail
                else
                    return AI_CheckBadMove(battlerAtk, battlerDef, predictedMove, score);
            }
            else
            {
                ADJUST_SCORE(-10); //Target is predicted to switch most likely
            }
            break;
        case EFFECT_NATURAL_GIFT:
            if (!IsBattlerItemEnabled(battlerAtk) || GetPocketByItemId(gBattleMons[battlerAtk].item) != POCKET_BERRIES)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_GRASSY_TERRAIN:
            if (PartnerMoveEffectIsTerrain(BATTLE_PARTNER(battlerAtk), aiData->partnerMove) || gFieldStatuses & STATUS_FIELD_GRASSY_TERRAIN)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_ELECTRIC_TERRAIN:
            if (PartnerMoveEffectIsTerrain(BATTLE_PARTNER(battlerAtk), aiData->partnerMove) || gFieldStatuses & STATUS_FIELD_ELECTRIC_TERRAIN)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_PSYCHIC_TERRAIN:
            if (PartnerMoveEffectIsTerrain(BATTLE_PARTNER(battlerAtk), aiData->partnerMove) || gFieldStatuses & STATUS_FIELD_PSYCHIC_TERRAIN)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_MISTY_TERRAIN:
            if (PartnerMoveEffectIsTerrain(BATTLE_PARTNER(battlerAtk), aiData->partnerMove) || gFieldStatuses & STATUS_FIELD_MISTY_TERRAIN)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_PLEDGE:
            if (isDoubleBattle && gBattleMons[BATTLE_PARTNER(battlerAtk)].hp > 0)
            {
                if (aiData->partnerMove != MOVE_NONE
                  && GetMoveEffect(aiData->partnerMove) == EFFECT_PLEDGE
                  && move != aiData->partnerMove) // Different pledge moves
                {
                    if (gBattleMons[BATTLE_PARTNER(battlerAtk)].status1 & (STATUS1_SLEEP | STATUS1_FREEZE))
                    // && gBattleMons[BATTLE_PARTNER(battlerAtk)].status1 != 1) // Will wake up this turn - how would AI know
                        ADJUST_SCORE(-10); // Don't use combo move if your partner will cause failure
                }
            }
            break;
        case EFFECT_TRICK_ROOM:
            if (PartnerMoveEffectIs(BATTLE_PARTNER(battlerAtk), aiData->partnerMove, EFFECT_TRICK_ROOM))
            {
                ADJUST_SCORE(-10);
            }
            else if (!(AI_THINKING_STRUCT->aiFlags[battlerAtk] & AI_FLAG_POWERFUL_STATUS))
            {
                if (gFieldStatuses & STATUS_FIELD_TRICK_ROOM) // Trick Room Up
                {
                    if (GetBattlerSideSpeedAverage(battlerAtk) < GetBattlerSideSpeedAverage(battlerDef)) // Attacker side slower than target side
                        ADJUST_SCORE(-10); // Keep the Trick Room up
                }
                else
                {
                    if (GetBattlerSideSpeedAverage(battlerAtk) >= GetBattlerSideSpeedAverage(battlerDef)) // Attacker side faster than target side
                        ADJUST_SCORE(-10); // Keep the Trick Room down
                }
            }
            break;
        case EFFECT_MAGIC_ROOM:
            if (gFieldStatuses & STATUS_FIELD_MAGIC_ROOM || PartnerMoveIsSameNoTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_WONDER_ROOM:
            if (gFieldStatuses & STATUS_FIELD_WONDER_ROOM || PartnerMoveIsSameNoTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_GRAVITY:
            if ((gFieldStatuses & STATUS_FIELD_GRAVITY
              && !IS_BATTLER_OF_TYPE(battlerAtk, TYPE_FLYING)
              && aiData->holdEffects[battlerAtk] != HOLD_EFFECT_AIR_BALLOON) // Should revert Gravity in this case
              || PartnerMoveIsSameNoTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_ION_DELUGE:
            if (gFieldStatuses & STATUS_FIELD_ION_DELUGE
              || PartnerMoveIsSameNoTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_FLING:
            if (!CanFling(battlerAtk))
            {
                ADJUST_SCORE(-10);
            }
            else
            {
                /* TODO Fling
                u8 effect = gFlingTable[gBattleMons[battlerAtk].item].effect;
                switch (effect)
                {
                case MOVE_EFFECT_BURN:
                    if (!AI_CanBurn(battlerAtk, battlerDef, BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                        ADJUST_SCORE(-10);
                    break;
                case MOVE_EFFECT_PARALYSIS:
                    if (!AI_CanParalyze(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, aiData->partnerMove))
                        ADJUST_SCORE(-10);
                    break;
                case MOVE_EFFECT_POISON:
                    if (!AI_CanPoison(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, aiData->partnerMove))
                        ADJUST_SCORE(-10);
                    break;
                case MOVE_EFFECT_TOXIC:
                    if (!AI_CanPoison(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, aiData->partnerMove))
                        ADJUST_SCORE(-10);
                    break;
                case MOVE_EFFECT_FREEZE:
                    if (!CanBeFrozen(battlerDef, TRUE)
                     || MoveBlockedBySubstitute(move, battlerAtk, battlerDef))
                        ADJUST_SCORE(-10);
                    break;
                }*/
            }
            break;
        case EFFECT_EMBARGO:
            if (!IsBattlerItemEnabled(battlerAtk)
              || gDisableStructs[battlerDef].embargoTimer != 0
              || PartnerMoveIsSameAsAttacker(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_POWDER:
            if (!HasMoveWithType(battlerDef, TYPE_FIRE)
              || PartnerMoveIsSameAsAttacker(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_TELEKINESIS:
            if (gStatuses3[battlerDef] & (STATUS3_TELEKINESIS | STATUS3_ROOTED | STATUS3_SMACKED_DOWN)
              || gFieldStatuses & STATUS_FIELD_GRAVITY
              || aiData->holdEffects[battlerDef] == HOLD_EFFECT_IRON_BALL
              || IsTelekinesisBannedSpecies(gBattleMons[battlerDef].species)
              || PartnerMoveIsSameAsAttacker(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_HEAL_BLOCK:
            if (gDisableStructs[battlerDef].healBlockTimer != 0
              || PartnerMoveIsSameAsAttacker(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SOAK:
        {
            u32 types[3];
            GetBattlerTypes(battlerDef, FALSE, types);
            // TODO: Use the type of the move like 'VARIOUS_TRY_SOAK'?
            if (PartnerMoveIsSameAsAttacker(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove)
              || (types[0] == TYPE_WATER && types[1] == TYPE_WATER && types[2] == TYPE_MYSTERY))
                ADJUST_SCORE(-10);    // target is already water-only
            break;
        }
        case EFFECT_THIRD_TYPE:
            if (IS_BATTLER_OF_TYPE(battlerDef, GetMoveArgType(move)) || PartnerMoveIsSameAsAttacker(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove) || GetActiveGimmick(battlerDef) == GIMMICK_TERA)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_HEAL_PULSE: // and floral healing
            if (!IS_TARGETING_PARTNER(battlerAtk, battlerDef)) // Don't heal enemies
            {
                ADJUST_SCORE(-10);
                break;
            }
            // fallthrough
        case EFFECT_HIT_ENEMY_HEAL_ALLY:    // pollen puff
            if (IS_TARGETING_PARTNER(battlerAtk, battlerDef))
            {
                if (gStatuses3[battlerDef] & STATUS3_HEAL_BLOCK)
                    return 0; // cannot even select
                if (AI_BattlerAtMaxHp(battlerDef))
                    ADJUST_SCORE(-10);
                else if (gBattleMons[battlerDef].hp > gBattleMons[battlerDef].maxHP / 2)
                    ADJUST_SCORE(-5);
            }
            break;
        case EFFECT_ELECTRIFY:
            if (AI_IsSlower(battlerAtk, battlerDef, move)
              //|| GetMoveTypeSpecial(battlerDef, predictedMove) == TYPE_ELECTRIC // Move will already be electric type
              || PartnerMoveIsSameAsAttacker(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_TOPSY_TURVY:
            if (!IS_TARGETING_PARTNER(battlerAtk, battlerDef))
            {
                u32 targetPositiveStages = CountPositiveStatStages(battlerDef);
                u32 targetNegativeStages = CountNegativeStatStages(battlerDef);

                if (targetPositiveStages == 0 //No good stat changes to make bad
                  || PartnerMoveIsSameAsAttacker(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                    ADJUST_SCORE(-10);

                else if (targetNegativeStages < targetPositiveStages)
                    ADJUST_SCORE(-5); //More stages would be made positive than negative
            }
            break;
        case EFFECT_FAIRY_LOCK:
            if ((gFieldStatuses & STATUS_FIELD_FAIRY_LOCK) || PartnerMoveIsSameNoTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_DO_NOTHING:
        case EFFECT_HOLD_HANDS:
        case EFFECT_CELEBRATE:
        case EFFECT_HAPPY_HOUR:
            ADJUST_SCORE(-10);
            break;
        case EFFECT_INSTRUCT:
            {
                u16 instructedMove;
                if (AI_IsSlower(battlerAtk, battlerDef, move))
                    instructedMove = predictedMove;
                else
                    instructedMove = gLastMoves[battlerDef];

                if (instructedMove == MOVE_NONE
                  || IsMoveInstructBanned(instructedMove)
                  || MoveHasAdditionalEffectSelf(instructedMove, MOVE_EFFECT_RECHARGE)
                  || IsZMove(instructedMove)
                  || (gLockedMoves[battlerDef] != 0 && gLockedMoves[battlerDef] != 0xFFFF)
                  || gBattleMons[battlerDef].status2 & STATUS2_MULTIPLETURNS
                  || PartnerMoveIsSameAsAttacker(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                {
                    ADJUST_SCORE(-10);
                }
                else if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
                    ADJUST_SCORE(-10);
                else if (isDoubleBattle)
                {
                    if (!IS_TARGETING_PARTNER(battlerAtk, battlerDef))
                        ADJUST_SCORE(-10);
                }
                else
                {
                    if (GetBattlerMoveTargetType(battlerDef, instructedMove) & (MOVE_TARGET_SELECTED
                                                             | MOVE_TARGET_DEPENDS
                                                             | MOVE_TARGET_RANDOM
                                                             | MOVE_TARGET_BOTH
                                                             | MOVE_TARGET_FOES_AND_ALLY
                                                             | MOVE_TARGET_OPPONENTS_FIELD)
                      && instructedMove != MOVE_MIND_BLOWN && instructedMove != MOVE_STEEL_BEAM)
                        ADJUST_SCORE(-10); //Don't force the enemy to attack you again unless it can kill itself with Mind Blown
                    else if (instructedMove != MOVE_MIND_BLOWN)
                        ADJUST_SCORE(-5); //Do something better
                }
            }
            break;
        case EFFECT_QUASH:
            if (!isDoubleBattle
            || AI_IsSlower(battlerAtk, battlerDef, move)
            || PartnerMoveIsSameAsAttacker(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_AFTER_YOU:
            if (!IS_TARGETING_PARTNER(battlerAtk, battlerDef)
              || !isDoubleBattle
              || AI_IsSlower(battlerAtk, battlerDef, move)
              || PartnerMoveIsSameAsAttacker(BATTLE_PARTNER(battlerAtk), battlerDef, move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SUCKER_PUNCH:
            if (predictedMove != MOVE_NONE)
            {
                if (IsBattleMoveStatus(predictedMove) || AI_IsSlower(battlerAtk, battlerDef, move)) // Opponent going first
                    ADJUST_SCORE(-10);
            }
            break;
        case EFFECT_TAILWIND:
            if (gSideStatuses[GetBattlerSide(battlerAtk)] & SIDE_STATUS_TAILWIND
             || PartnerMoveEffectIs(BATTLE_PARTNER(battlerAtk), aiData->partnerMove, EFFECT_TAILWIND)
             || gFieldStatuses & STATUS_FIELD_TRICK_ROOM)
                ADJUST_SCORE(-10);
            break;
        case EFFECT_LUCKY_CHANT:
            if (gSideStatuses[GetBattlerSide(battlerAtk)] & SIDE_STATUS_LUCKY_CHANT
              || PartnerMoveIsSameNoTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_MAGNET_RISE:
            if (gFieldStatuses & STATUS_FIELD_GRAVITY
              ||  gDisableStructs[battlerAtk].magnetRiseTimer != 0
              || aiData->holdEffects[battlerAtk] == HOLD_EFFECT_IRON_BALL
              || gStatuses3[battlerAtk] & (STATUS3_ROOTED | STATUS3_MAGNET_RISE | STATUS3_SMACKED_DOWN)
              || !IsBattlerGrounded(battlerAtk))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_CAMOUFLAGE:
            if (!CanCamouflage(battlerAtk))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SYNCHRONOISE:
            //Check holding ring target or is of same type
            if (aiData->holdEffects[battlerDef] == HOLD_EFFECT_RING_TARGET
              || DoBattlersShareType(battlerAtk, battlerDef))
                break;
            else
                ADJUST_SCORE(-10);
            break;
        case EFFECT_FLAIL:
            if (AI_IsSlower(battlerAtk, battlerDef, move) // Opponent should go first
             || aiData->hpPercents[battlerAtk] > 50)
                ADJUST_SCORE(-4);
            break;
        //TODO
        //case EFFECT_SHELL_TRAP:
            //break;
        //case EFFECT_BEAK_BLAST:
            //break;
        case EFFECT_SKY_DROP:
            if (IS_BATTLER_OF_TYPE(battlerDef, TYPE_FLYING))
                ADJUST_SCORE(-10);
            if (BattlerWillFaintFromWeather(battlerAtk, aiData->abilities[battlerAtk])
            ||  DoesSubstituteBlockMove(battlerAtk, battlerDef, move)
            ||  GetBattlerWeight(battlerDef) >= 2000) //200.0 kg
                ADJUST_SCORE(-10);
            break;
        /*case EFFECT_NO_RETREAT:
            if (TrappedByNoRetreat(battlerAtk))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_EXTREME_EVOBOOST:
            if (MainStatsMaxed(battlerAtk))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_CLANGOROUS_SOUL:
            if (gBattleMons[battlerAtk].hp <= gBattleMons[battlerAtk].maxHP / 3)
                ADJUST_SCORE(-10);
            break;*/
        case EFFECT_REVIVAL_BLESSING:
            if (GetFirstFaintedPartyIndex(battlerAtk) == PARTY_SIZE)
                ADJUST_SCORE(-10);
            else if (CanAIFaintTarget(battlerAtk, battlerDef, 0))
                ADJUST_SCORE(-10);
            else if (CanTargetFaintAi(battlerDef, battlerAtk)
             && AI_IsSlower(battlerAtk, battlerDef, move))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_JUNGLE_HEALING:
           if (AI_BattlerAtMaxHp(battlerAtk)
            && AI_BattlerAtMaxHp(BATTLE_PARTNER(battlerAtk))
            && !(gBattleMons[battlerAtk].status1 & STATUS1_ANY)
            && !(gBattleMons[BATTLE_PARTNER(battlerAtk)].status1 & STATUS1_ANY))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_TAKE_HEART:
            if ((!(gBattleMons[battlerAtk].status1 & STATUS1_ANY)
             || PartnerMoveEffectIs(BATTLE_PARTNER(battlerAtk), aiData->partnerMove, EFFECT_JUNGLE_HEALING)
             || PartnerMoveEffectIs(BATTLE_PARTNER(battlerAtk), aiData->partnerMove, EFFECT_HEAL_BELL))
             && !BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPATK)
             && !BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPDEF))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_SPICY_EXTRACT:
            if (battlerAtk != BATTLE_PARTNER(battlerDef)
             && (HasMoveWithCategory(battlerDef, DAMAGE_CATEGORY_PHYSICAL)
              || aiData->abilities[battlerDef] == ABILITY_CLEAR_BODY
              || aiData->abilities[battlerDef] == ABILITY_GOOD_AS_GOLD
              || aiData->holdEffects[battlerDef] == HOLD_EFFECT_CLEAR_AMULET))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_UPPER_HAND:
            if (predictedMove == MOVE_NONE || IsBattleMoveStatus(predictedMove) || AI_IsSlower(battlerAtk, battlerDef, move) || GetBattleMovePriority(battlerDef, predictedMove) < 1 || GetBattleMovePriority(battlerDef, predictedMove) > 3) // Opponent going first or not using priority move
            {
                ADJUST_SCORE(-10);
            }    
            break;
        case EFFECT_PLACEHOLDER:
            return 0;   // cannot even select
    } // move effect checks

    // Choice items
    if (HOLD_EFFECT_CHOICE(aiData->holdEffects[battlerAtk]) && IsBattlerItemEnabled(battlerAtk))
    {
        // Don't use user-target moves ie. Swords Dance, with exceptions
        if ((moveTarget & MOVE_TARGET_USER)
        && moveEffect != EFFECT_DESTINY_BOND && moveEffect != EFFECT_WISH && moveEffect != EFFECT_HEALING_WISH
        && !(moveEffect == EFFECT_AURORA_VEIL && (AI_GetWeather() & (B_WEATHER_SNOW | B_WEATHER_HAIL))))
            ADJUST_SCORE(-30);
        // Don't use a status move if the mon is the last one in the party, has no good switchin, or is trapped
        else if (GetBattleMoveCategory(move) == DAMAGE_CATEGORY_STATUS
            && (CountUsablePartyMons(battlerAtk) < 1
            || AI_DATA->mostSuitableMonId[battlerAtk] == PARTY_SIZE
            || IsBattlerTrapped(battlerAtk, TRUE)))
            ADJUST_SCORE(-30);
    }

    return score;
}

static s32 AI_TryToFaint(u32 battlerAtk, u32 battlerDef, u32 move)
{
    u32 score = 0;
    u32 movesetIndex = AI_THINKING_STRUCT->movesetIndex;
    s8 atkPriority = GetBattleMovePriority(battlerAtk, move);
    struct AiLogicData *aiData = AI_DATA;

    if (IsBattleMoveStatus(move))
        return score; // status moves aren't accounted here

    if (CanIndexMoveFaintTarget(battlerAtk, battlerDef, movesetIndex, 0) && GetMoveEffect(move) != EFFECT_EXPLOSION)
    {
        // DebugPrintf("Can knock out!?");
        switch (aiData->abilities[battlerAtk])
        {
            case ABILITY_BEAST_BOOST:
            case ABILITY_CHILLING_NEIGH:
            case ABILITY_GRIM_NEIGH:
            case ABILITY_MOXIE:
                ADJUST_SCORE(1);
                break;
        }
        if (atkPriority > 0)
            ADJUST_SCORE(8);
        else if (AI_IsFaster(battlerAtk, battlerDef, move))
        {
            if (move == MOVE_PURSUIT)
                ADJUST_SCORE(9);
            else
                ADJUST_SCORE(6);
        }
        else
            if (!CanTargetFaintAi(battlerDef, battlerAtk) && move == MOVE_PURSUIT)
                ADJUST_SCORE(6);
            else
                ADJUST_SCORE(4);
    }
    else if (CanTargetFaintAi(battlerDef, battlerAtk)
            && GetWhichBattlerFasterOrTies(battlerAtk, battlerDef, TRUE) != AI_IS_FASTER
            && GetBattleMovePriority(battlerAtk, move) > 0)
    {
        // DebugPrintf("I can be knocked out!?");
        ADJUST_SCORE(8);
    }

    return score;
}

// double battle logic
static s32 AI_DoubleBattle(u32 battlerAtk, u32 battlerDef, u32 move, s32 score)
{
    // move data
    u32 moveType = GetMoveType(move);
    u32 effect = GetMoveEffect(move);
    u32 moveTarget = GetBattlerMoveTargetType(battlerAtk, move);
    // ally data
    u32 battlerAtkPartner = BATTLE_PARTNER(battlerAtk);
    struct AiLogicData *aiData = AI_DATA;
    u32 atkPartnerAbility = aiData->abilities[BATTLE_PARTNER(battlerAtk)];
    u32 atkPartnerHoldEffect = aiData->holdEffects[BATTLE_PARTNER(battlerAtk)];
    u32 partnerEffect = GetMoveEffect(aiData->partnerMove);
    bool32 partnerProtecting = (partnerEffect == EFFECT_PROTECT);
    bool32 attackerHasBadAbility = (gAbilitiesInfo[aiData->abilities[battlerAtk]].aiRating < 0);
    bool32 partnerHasBadAbility = (gAbilitiesInfo[atkPartnerAbility].aiRating < 0);
    u32 predictedMove = aiData->lastUsedMove[battlerDef];

    SetTypeBeforeUsingMove(move, battlerAtk);
    moveType = GetBattleMoveType(move);

    // check what effect partner is using
    if (aiData->partnerMove != 0)
    {
        switch (partnerEffect)
        {
        case EFFECT_HELPING_HAND:
            if (IsBattleMoveStatus(move))
                ADJUST_SCORE(-10);
            break;
        case EFFECT_PERISH_SONG:
            if (!(gBattleMons[battlerDef].status2 & (STATUS2_ESCAPE_PREVENTION | STATUS2_WRAPPED)))
            {
                if (IsTrappingMove(aiData->partnerMove) || predictedMove == MOVE_INGRAIN)
                    ADJUST_SCORE(WEAK_EFFECT);
            }
            break;
        // Don't change weather if ally already decided to do so.
        case EFFECT_SUNNY_DAY:
        case EFFECT_HAIL:
        case EFFECT_SNOWSCAPE:
        case EFFECT_RAIN_DANCE:
        case EFFECT_SANDSTORM:
        case EFFECT_CHILLY_RECEPTION:
            if (IsMoveEffectWeather(move))
                ADJUST_SCORE(-10);
            break;
        }
    } // check partner move effect

    // Adjust for always crit moves
    if (MoveAlwaysCrits(aiData->partnerMove) && aiData->abilities[battlerAtk] == ABILITY_ANGER_POINT)
    {
        if (AI_IsSlower(battlerAtk, battlerAtkPartner, move))   // Partner moving first
        {
            // discourage raising our attack since it's about to be maxed out
            if (IsAttackBoostMoveEffect(effect))
                ADJUST_SCORE(-3);
            // encourage moves hitting multiple opponents
            if (!IsBattleMoveStatus(move) && (moveTarget & (MOVE_TARGET_BOTH | MOVE_TARGET_FOES_AND_ALLY)))
                ADJUST_SCORE(GOOD_EFFECT);
        }
    }

    // consider our move effect relative to partner state
    switch (effect)
    {
    case EFFECT_HELPING_HAND:
        if (!IsBattlerAlive(battlerAtkPartner) || !HasDamagingMove(battlerAtkPartner))
            ADJUST_SCORE(-5);
        break;
    case EFFECT_PERISH_SONG:
        if (aiData->partnerMove != 0 && HasTrappingMoveEffect(battlerAtkPartner))
            ADJUST_SCORE(WEAK_EFFECT);
        break;
    case EFFECT_MAGNET_RISE:
        if (IsBattlerGrounded(battlerAtk)
          && (HasMove(battlerAtkPartner, MOVE_EARTHQUAKE) || HasMove(battlerAtkPartner, MOVE_MAGNITUDE))
          && (AI_GetMoveEffectiveness(MOVE_EARTHQUAKE, battlerAtk, battlerAtkPartner) != UQ_4_12(0.0))) // Doesn't resist ground move
        {
            RETURN_SCORE_PLUS(DECENT_EFFECT);   // partner has earthquake or magnitude -> good idea to use magnet rise
        }
        break;
    case EFFECT_DRAGON_CHEER:
        if (gBattleMons[battlerAtkPartner].status2 & STATUS2_FOCUS_ENERGY_ANY || !HasDamagingMove(battlerAtkPartner))
            ADJUST_SCORE(-5);
        else if (atkPartnerHoldEffect == HOLD_EFFECT_SCOPE_LENS
              || IS_BATTLER_OF_TYPE(battlerAtkPartner, TYPE_DRAGON)
              || GetMoveCriticalHitStage(aiData->partnerMove) > 0
              || HasMoveWithCriticalHitChance(battlerAtkPartner))
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    } // our effect relative to partner

    // consider global move effects
    switch (effect)
    {
    case EFFECT_SANDSTORM:
        if (ShouldSetSandstorm(battlerAtkPartner, atkPartnerAbility, atkPartnerHoldEffect))
        {
            RETURN_SCORE_PLUS(WEAK_EFFECT);   // our partner benefits from sandstorm
        }
        break;
    case EFFECT_RAIN_DANCE:
        if (ShouldSetRain(battlerAtkPartner, atkPartnerAbility, atkPartnerHoldEffect))
        {
            RETURN_SCORE_PLUS(WEAK_EFFECT);   // our partner benefits from rain
        }
        break;
    case EFFECT_SUNNY_DAY:
        if (ShouldSetSun(battlerAtkPartner, atkPartnerAbility, atkPartnerHoldEffect))
        {
            RETURN_SCORE_PLUS(WEAK_EFFECT);   // our partner benefits from sun
        }
        break;
    case EFFECT_HAIL:
        if (IsBattlerAlive(battlerAtkPartner)
         && ShouldSetHail(battlerAtkPartner, atkPartnerAbility, atkPartnerHoldEffect))
        {
            RETURN_SCORE_PLUS(DECENT_EFFECT);   // our partner benefits from hail
        }
        break;
    case EFFECT_SNOWSCAPE:
    case EFFECT_CHILLY_RECEPTION:
        if (IsBattlerAlive(battlerAtkPartner)
         && ShouldSetSnow(battlerAtkPartner, atkPartnerAbility, atkPartnerHoldEffect))
        {
            RETURN_SCORE_PLUS(DECENT_EFFECT);   // our partner benefits from snow
        }
        break;
    } // global move effect check

    // check specific target
    if (IS_TARGETING_PARTNER(battlerAtk, battlerDef))
    {
        // partner ability checks
        if (!partnerProtecting && moveTarget != MOVE_TARGET_BOTH && !DoesBattlerIgnoreAbilityChecks(battlerAtk, aiData->abilities[battlerAtk], move))
        {
            switch (atkPartnerAbility)
            {
            case ABILITY_ANGER_POINT:
                if (MoveAlwaysCrits(move)
                    && BattlerStatCanRise(battlerAtkPartner, atkPartnerAbility, STAT_ATK)
                    && AI_IsFaster(battlerAtk, battlerAtkPartner, move)
                    && !CanIndexMoveFaintTarget(battlerAtk, battlerAtkPartner, AI_THINKING_STRUCT->movesetIndex, 1))
                {
                    RETURN_SCORE_PLUS(GOOD_EFFECT);
                }
                break;
            case ABILITY_VOLT_ABSORB:
                if (!(AI_THINKING_STRUCT->aiFlags[battlerAtk] & AI_FLAG_HP_AWARE))
                {
                    RETURN_SCORE_MINUS(10);
                }
                break;  // TODO This mentioned AI_HPAware
            case ABILITY_MOTOR_DRIVE:
                if (moveType == TYPE_ELECTRIC && BattlerStatCanRise(battlerAtkPartner, atkPartnerAbility, STAT_SPEED))
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case ABILITY_LIGHTNING_ROD:
                if (B_REDIRECT_ABILITY_IMMUNITY >= GEN_5
                    && moveType == TYPE_ELECTRIC
                    && HasMoveWithCategory(battlerAtkPartner, DAMAGE_CATEGORY_SPECIAL)
                    && BattlerStatCanRise(battlerAtkPartner, atkPartnerAbility, STAT_SPATK))
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case ABILITY_WATER_ABSORB:
            case ABILITY_DRY_SKIN:
            case ABILITY_EARTH_EATER:
                if (!(AI_THINKING_STRUCT->aiFlags[battlerAtk] & AI_FLAG_HP_AWARE))
                {
                    RETURN_SCORE_MINUS(10);
                }
                break;  // TODO This mentioned AI_HPAware
            case ABILITY_STORM_DRAIN:
                if (B_REDIRECT_ABILITY_IMMUNITY >= GEN_5
                    && moveType == TYPE_WATER
                    && HasMoveWithCategory(battlerAtkPartner, DAMAGE_CATEGORY_SPECIAL)
                    && BattlerStatCanRise(battlerAtkPartner, atkPartnerAbility, STAT_SPATK))
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case ABILITY_WATER_COMPACTION:
                if (moveType == TYPE_WATER && GetNoOfHitsToKOBattler(battlerAtk, battlerDef, AI_THINKING_STRUCT->movesetIndex) >= 4)
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);   // only mon with this ability is weak to water so only make it okay if we do very little damage
                }
                RETURN_SCORE_MINUS(10);
                break;
            case ABILITY_FLASH_FIRE:
                if (moveType == TYPE_FIRE
                    && HasMoveWithType(battlerAtkPartner, TYPE_FIRE)
                    && !gDisableStructs[battlerAtkPartner].flashFireBoosted)
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case ABILITY_SAP_SIPPER:
                if (moveType == TYPE_GRASS
                    && HasMoveWithCategory(battlerAtkPartner, DAMAGE_CATEGORY_PHYSICAL)
                    && BattlerStatCanRise(battlerAtkPartner, atkPartnerAbility, STAT_ATK))
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case ABILITY_JUSTIFIED:
                if (moveType == TYPE_DARK
                    && !IsBattleMoveStatus(move)
                    && HasMoveWithCategory(battlerAtkPartner, DAMAGE_CATEGORY_PHYSICAL)
                    && BattlerStatCanRise(battlerAtkPartner, atkPartnerAbility, STAT_ATK)
                    && !CanIndexMoveFaintTarget(battlerAtk, battlerAtkPartner, AI_THINKING_STRUCT->movesetIndex, 1))
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case ABILITY_RATTLED:
                if (!IsBattleMoveStatus(move)
                    && (moveType == TYPE_DARK || moveType == TYPE_GHOST || moveType == TYPE_BUG)
                    && BattlerStatCanRise(battlerAtkPartner, atkPartnerAbility, STAT_SPEED)
                    && !CanIndexMoveFaintTarget(battlerAtk, battlerAtkPartner, AI_THINKING_STRUCT->movesetIndex, 1))
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case ABILITY_CONTRARY:
                if (IsStatLoweringEffect(effect))
                {
                    RETURN_SCORE_PLUS(DECENT_EFFECT);
                }
                break;
            }
        } // ability checks

        // attacker move effects specifically targeting partner
        if (!partnerProtecting)
        {
            switch (effect)
            {
            case EFFECT_SPICY_EXTRACT:
                if (AI_ShouldSpicyExtract(battlerAtk, battlerAtkPartner, move, aiData))
                {
                    RETURN_SCORE_PLUS(GOOD_EFFECT);
                }
                break;
            case EFFECT_PURIFY:
                if (gBattleMons[battlerAtkPartner].status1 & STATUS1_ANY)
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case EFFECT_SWAGGER:
                if (gBattleMons[battlerAtkPartner].statStages[STAT_ATK] < MAX_STAT_STAGE
                 && HasMoveWithCategory(battlerAtkPartner, DAMAGE_CATEGORY_PHYSICAL)
                 && (!AI_CanBeConfused(battlerAtk, battlerAtkPartner, move, atkPartnerAbility)
                  || atkPartnerHoldEffect == HOLD_EFFECT_CURE_CONFUSION
                  || atkPartnerHoldEffect == HOLD_EFFECT_CURE_STATUS))
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case EFFECT_FLATTER:
                if (gBattleMons[battlerAtkPartner].statStages[STAT_SPATK] < MAX_STAT_STAGE
                 && HasMoveWithCategory(battlerAtkPartner, DAMAGE_CATEGORY_SPECIAL)
                 && (!AI_CanBeConfused(battlerAtk, battlerAtkPartner, move, atkPartnerAbility)
                  || atkPartnerHoldEffect == HOLD_EFFECT_CURE_CONFUSION
                  || atkPartnerHoldEffect == HOLD_EFFECT_CURE_STATUS))
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case EFFECT_BEAT_UP:
                if (atkPartnerAbility == ABILITY_JUSTIFIED
                  && moveType == TYPE_DARK
                  && !IsBattleMoveStatus(move)
                  && HasMoveWithCategory(battlerAtkPartner, DAMAGE_CATEGORY_PHYSICAL)
                  && BattlerStatCanRise(battlerAtkPartner, atkPartnerAbility, STAT_ATK)
                  && !CanIndexMoveFaintTarget(battlerAtk, battlerAtkPartner, AI_THINKING_STRUCT->movesetIndex, 0))
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case EFFECT_SKILL_SWAP:
                if (aiData->abilities[battlerAtk] != aiData->abilities[BATTLE_PARTNER(battlerAtk)] && !attackerHasBadAbility)
                {
                    // Partner abilities
                    if (aiData->abilities[BATTLE_PARTNER(battlerAtk)] == ABILITY_TRUANT)
                    {
                        ADJUST_SCORE(10);
                    }
                    else if (aiData->abilities[BATTLE_PARTNER(battlerAtk)] == ABILITY_INTIMIDATE)
                    {
                        ADJUST_SCORE(DECENT_EFFECT);
                    }
                    // Active mon abilities
                    if (aiData->abilities[battlerAtk] == ABILITY_COMPOUND_EYES
                        && HasMoveWithLowAccuracy(battlerAtkPartner, FOE(battlerAtkPartner), 90, TRUE, atkPartnerAbility, aiData->abilities[FOE(battlerAtkPartner)], atkPartnerHoldEffect, aiData->holdEffects[FOE(battlerAtkPartner)]))
                    {
                        ADJUST_SCORE(GOOD_EFFECT);
                    }
                    else if (aiData->abilities[battlerAtk] == ABILITY_CONTRARY && HasMoveThatLowersOwnStats(battlerAtkPartner))
                    {
                        ADJUST_SCORE(GOOD_EFFECT);
                    }
                    return score;
                }
                break;
            case EFFECT_ROLE_PLAY:
                if (attackerHasBadAbility && !partnerHasBadAbility)
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case EFFECT_WORRY_SEED:
            case EFFECT_GASTRO_ACID:
            case EFFECT_SIMPLE_BEAM:
                if (partnerHasBadAbility)
                {
                    RETURN_SCORE_PLUS(DECENT_EFFECT);
                }
                break;
            case EFFECT_ENTRAINMENT:
                if (partnerHasBadAbility && IsAbilityOfRating(aiData->abilities[battlerAtk], 0))
                {
                    RETURN_SCORE_PLUS(5);
                }
                break;
            case EFFECT_SOAK:
                if (atkPartnerAbility == ABILITY_WONDER_GUARD
                 && !IS_BATTLER_OF_TYPE(battlerAtkPartner, TYPE_WATER)
                 && GetActiveGimmick(battlerAtkPartner) != GIMMICK_TERA)
                {
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case EFFECT_INSTRUCT:
                {
                    u16 instructedMove;
                    if (AI_IsFaster(battlerAtk, battlerAtkPartner, move))
                        instructedMove = aiData->partnerMove;
                    else
                        instructedMove = gLastMoves[battlerAtkPartner];

                    if (instructedMove != MOVE_NONE
                      && !IsBattleMoveStatus(instructedMove)
                      && (GetBattlerMoveTargetType(battlerAtkPartner, instructedMove) & (MOVE_TARGET_BOTH | MOVE_TARGET_FOES_AND_ALLY))) // Use instruct on multi-target moves
                    {
                        RETURN_SCORE_PLUS(WEAK_EFFECT);
                    }
                }
                break;
            case EFFECT_AFTER_YOU:
                if (AI_IsSlower(battlerAtkPartner, FOE(battlerAtkPartner), aiData->partnerMove)  // Opponent mon 1 goes before partner
                 || AI_IsSlower(battlerAtkPartner, BATTLE_PARTNER(FOE(battlerAtkPartner)), aiData->partnerMove)) // Opponent mon 2 goes before partner
                {
                    if (partnerEffect == EFFECT_COUNTER || partnerEffect == EFFECT_MIRROR_COAT)
                        break; // These moves need to go last
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                }
                break;
            case EFFECT_HEAL_PULSE:
            case EFFECT_HIT_ENEMY_HEAL_ALLY:
                if (AI_IsFaster(battlerAtk, FOE(battlerAtk), move)
                 && AI_IsFaster(battlerAtk, BATTLE_PARTNER(FOE(battlerAtk)), move)
                 && gBattleMons[battlerAtkPartner].hp < gBattleMons[battlerAtkPartner].maxHP / 2)
                    RETURN_SCORE_PLUS(WEAK_EFFECT);
                break;
            } // attacker move effects
        } // check partner protecting

        // DebugPrintf("Otherwise don't target partner");
        ADJUST_SCORE(-30); // otherwise, don't target partner
    }
    else // checking opponent
    {
        switch (effect)
        {
        case EFFECT_SKILL_SWAP:
            if (aiData->abilities[battlerAtk] == ABILITY_TRUANT)
                ADJUST_SCORE(GOOD_EFFECT);
            else if (IsAbilityOfRating(aiData->abilities[battlerAtk], 0) || IsAbilityOfRating(aiData->abilities[battlerDef], 10))
                ADJUST_SCORE(DECENT_EFFECT); // we want to transfer our bad ability or take their awesome ability
            break;
        }
    }

    return score;
}

static bool32 IsPinchBerryItemEffect(u32 holdEffect)
{
    switch (holdEffect)
    {
    case HOLD_EFFECT_ATTACK_UP:
    case HOLD_EFFECT_DEFENSE_UP:
    case HOLD_EFFECT_SPEED_UP:
    case HOLD_EFFECT_SP_ATTACK_UP:
    case HOLD_EFFECT_SP_DEFENSE_UP:
    case HOLD_EFFECT_CRITICAL_UP:
    case HOLD_EFFECT_RANDOM_STAT_UP:
    case HOLD_EFFECT_CUSTAP_BERRY:
    case HOLD_EFFECT_MICLE_BERRY:
        return TRUE;
    }

    return FALSE;
}

static s32 CompareMoveAccuracies(u32 battlerAtk, u32 battlerDef, u32 moveSlot1, u32 moveSlot2)
{
    u32 acc1 = AI_DATA->moveAccuracy[battlerAtk][battlerDef][moveSlot1];
    u32 acc2 = AI_DATA->moveAccuracy[battlerAtk][battlerDef][moveSlot2];

    if (acc1 > acc2)
        return 1;
    else if (acc2 > acc1)
        return -1;
    return 0;
}

static inline bool32 ShouldUseSpreadDamageMove(u32 battlerAtk, u32 move, u32 moveIndex, u32 hitsToFaintOpposingBattler)
{
    u32 partnerBattler = BATTLE_PARTNER(battlerAtk);
    u32 noOfHitsToFaintPartner = GetNoOfHitsToKOBattler(battlerAtk, partnerBattler, moveIndex);
    return (IsDoubleBattle()
         && noOfHitsToFaintPartner != 0 // Immunity check
         && IsBattlerAlive(partnerBattler)
         && GetBattlerMoveTargetType(battlerAtk, move) == MOVE_TARGET_FOES_AND_ALLY
         && !(noOfHitsToFaintPartner < 4 && hitsToFaintOpposingBattler == 1)
         && noOfHitsToFaintPartner < 7);
}

static s32 AI_CompareDamagingMoves(u32 battlerAtk, u32 battlerDef, u32 currId)
{
    u32 i;
    bool32 multipleBestMoves = FALSE;
    s32 viableMoveScores[MAX_MON_MOVES];
    s32 bestViableMoveScore;
    s32 noOfHits[MAX_MON_MOVES];
    s32 score = 0;
    s32 leastHits = 1000;
    u16 *moves = GetMovesArray(battlerAtk);
    bool8 isTwoTurnNotSemiInvulnerableMove[MAX_MON_MOVES];

    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        if (moves[i] != MOVE_NONE && GetMovePower(moves[i]) != 0)
        {
            noOfHits[i] = GetNoOfHitsToKOBattler(battlerAtk, battlerDef, i);
            // DebugPrintf("noOfHits[i]: %d", noOfHits[i]);
            if (ShouldUseSpreadDamageMove(battlerAtk,moves[i], i, noOfHits[i]))
            {
                // DebugPrintf("Should I spread?");
                noOfHits[i] = -1;
                viableMoveScores[i] = 0;
                isTwoTurnNotSemiInvulnerableMove[i] = FALSE;
            }
            else if (noOfHits[i] < leastHits && noOfHits[i] != 0)
            {
                // DebugPrintf("leasthits wizardry");
                leastHits = noOfHits[i];
            }
            viableMoveScores[i] = AI_SCORE_DEFAULT;
            isTwoTurnNotSemiInvulnerableMove[i] = IsTwoTurnNotSemiInvulnerableMove(battlerAtk, moves[i]);
            // DebugPrintf("End of block viableMoveScores[i]: %d", viableMoveScores[i]);
        }
        else
        {
            noOfHits[i] = -1;
            viableMoveScores[i] = 0;
            isTwoTurnNotSemiInvulnerableMove[i] = FALSE;
        }
        /*
            Test_MgbaPrintf("%S: required hits: %d Dmg: %d", gMoveNames[moves[i]], noOfHits[i], AI_DATA->simulatedDmg[battlerAtk][battlerDef][i]);
        */
    }

    // Priority list:
    // 1. Less no of hits to ko
    // 2. Not charging
    // 3. More accuracy
    // 4. Better effect

    // Current move requires the least hits to KO. Compare with other moves.
    if (leastHits == noOfHits[currId])
    {
        // DebugPrintf("We leasthitting! %d", viableMoveScores[currId]);
        for (i = 0; i < MAX_MON_MOVES; i++)
        {
            if (i == currId)
                continue;
            if (noOfHits[currId] == noOfHits[i])
            {
                multipleBestMoves = TRUE;
                // We need to make sure it's the current move which is objectively better.
                if (isTwoTurnNotSemiInvulnerableMove[i] && !isTwoTurnNotSemiInvulnerableMove[currId])
                    viableMoveScores[i] -= 3;
                else if (!isTwoTurnNotSemiInvulnerableMove[i] && isTwoTurnNotSemiInvulnerableMove[currId])
                    viableMoveScores[currId] -= 3;

                switch (CompareMoveAccuracies(battlerAtk, battlerDef, currId, i))
                {
                case 1:
                    viableMoveScores[i] -= 2;
                    break;
                case -1:
                    viableMoveScores[currId] -= 2;
                    break;
                }
                // DebugPrintf("Checking on currId: %d", viableMoveScores[currId]);
                // DebugPrintf("Checking on i: %d", viableMoveScores[i]);
                switch (AI_WhichMoveBetter(moves[currId], moves[i], battlerAtk, battlerDef, noOfHits[currId]))
                {
                case 1:
                    viableMoveScores[i] -= 1;
                    break;
                case -1:
                    viableMoveScores[currId] -= 1;
                    break;
                }
                // DebugPrintf("Checking on currId: %d", viableMoveScores[currId]);
                // DebugPrintf("Checking on i: %d", viableMoveScores[i]);


                switch (AI_WhichMoveDealsMoreDamage(currId, i, battlerAtk, battlerDef))
                {
                    case 1:
                        viableMoveScores[i] -= 1;
                        break;
                    case -1:
                        viableMoveScores[currId] -= 1;
                        break;
                }
            }
        }
        // Turns out the current move deals the most dmg compared to the other 3.
        // DebugPrintf("Multiple Best Moves?: %d", multipleBestMoves);
        if (!multipleBestMoves)
            ADJUST_SCORE(3);
        else
        {
            bestViableMoveScore = 0;
            for (i = 0; i < MAX_MON_MOVES; i++)
            {
                // DebugPrintf("viableMoveScore[i]: %d", viableMoveScores[i]);
                if (viableMoveScores[i] > bestViableMoveScore)
                    bestViableMoveScore = viableMoveScores[i];
            }
            // Unless a better move was found increase score of current move
            // DebugPrintf("viableMoveScores[currId]: %d", viableMoveScores[currId]);
            if (viableMoveScores[currId] == bestViableMoveScore)
                ADJUST_SCORE(3);
        }
    }

    return score;
}

static u32 AI_CalcHoldEffectMoveScore(u32 battlerAtk, u32 battlerDef, u32 move)
{
    struct AiLogicData *aiData = AI_DATA;
    u32 holdEffect = aiData->holdEffects[battlerAtk];

    s32 score = 0;

    switch (holdEffect)
    {
    case HOLD_EFFECT_BLUNDER_POLICY:
    {
        u32 moveAcc = aiData->moveAccuracy[battlerAtk][battlerDef][AI_THINKING_STRUCT->movesetIndex];

        if (moveAcc <= LOW_ACCURACY_THRESHOLD)
        {
            ADJUST_SCORE(GOOD_EFFECT);
        }
        else
        {
            ADJUST_SCORE(-DECENT_EFFECT);
        }
    }
    break;
    }

    return score;
}

static u32 AI_CalcMoveEffectScore(u32 battlerAtk, u32 battlerDef, u32 move)
{
    // DebugPrintf("Entering AI_CalcMoveEffectScore");
    // move data
    u32 moveEffect = GetMoveEffect(move);
    struct AiLogicData *aiData = AI_DATA;
    u32 movesetIndex = AI_THINKING_STRUCT->movesetIndex;
    uq4_12_t effectiveness = aiData->effectiveness[battlerAtk][battlerDef][movesetIndex];

    s32 score = 0;
    u32 predictedMove = aiData->lastUsedMove[battlerDef];
    u32 predictedType = GetMoveType(predictedMove);
    u32 predictedMoveSlot = GetMoveSlot(GetMovesArray(battlerDef), predictedMove);
    bool32 isDoubleBattle = IsValidDoubleBattle(battlerAtk);
    u32 abilityDef = AI_DATA->abilities[battlerDef];
    u32 i;

    // The AI should understand that while Dynamaxed, status moves function like Protect.
    if (GetActiveGimmick(battlerAtk) == GIMMICK_DYNAMAX && GetMoveCategory(move) == DAMAGE_CATEGORY_STATUS)
        moveEffect = EFFECT_PROTECT;

    // check thawing moves
    if ((gBattleMons[battlerAtk].status1 & (STATUS1_FREEZE | STATUS1_FROSTBITE)) && MoveThawsUser(move))
        ADJUST_SCORE(30);

    // check status conditions with natural cure
    if (AI_DATA->abilities[battlerAtk] == ABILITY_NATURAL_CURE)
    {
        if ((gBattleMons[battlerAtk].status1 & STATUS1_BURN && HasOnlyMovesWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL, TRUE))
        || (gBattleMons[battlerAtk].status1 & STATUS1_FROSTBITE && HasOnlyMovesWithCategory(battlerAtk, DAMAGE_CATEGORY_SPECIAL, TRUE))
        || (gBattleMons[battlerAtk].status1 & STATUS1_SLEEP) || (gBattleMons[battlerAtk].status1 & STATUS1_TOXIC_POISON) 
        || (gBattleMons[battlerAtk].status1 & STATUS1_PARALYSIS) || (gBattleMons[battlerAtk].status1 & STATUS1_FREEZE))
            ADJUST_SCORE(-20); // Force switch if all your attacking moves are physical and you have Natural Cure.
    }

    // move effect checks
    switch (moveEffect)
    {
    case EFFECT_SLEEP:
        if (GetMoveAccuracy(move) == 100 && (AI_IsFaster(battlerAtk, battlerDef, move) || !CanTargetFaintAi(battlerDef, battlerAtk)))
            ADJUST_SCORE(5);
        else if (GetMoveAccuracy(move) == 100)
            ADJUST_SCORE(GOOD_EFFECT);
        else
            IncreaseSleepScore(battlerAtk, battlerDef, move, &score);
        break;
    case EFFECT_YAWN:
        IncreaseSleepScore(battlerAtk, battlerDef, move, &score);
        break;
    case EFFECT_ABSORB:
        if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_BIG_ROOT && effectiveness >= UQ_4_12(1.0))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_DREAM_EATER:
    case EFFECT_STRENGTH_SAP:
    case EFFECT_AQUA_RING:
        if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_BIG_ROOT)
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_EXPLOSION:
    case EFFECT_MEMENTO:
        if (AI_THINKING_STRUCT->aiFlags[battlerAtk] & AI_FLAG_WILL_SUICIDE && gBattleMons[battlerDef].statStages[STAT_EVASION] < 7)
        {
            if (aiData->hpPercents[battlerAtk] < 50 && AI_RandLessThan(128))
                ADJUST_SCORE(DECENT_EFFECT);
        }
        break;
    case EFFECT_FINAL_GAMBIT:
        if (AI_THINKING_STRUCT->aiFlags[battlerAtk] & AI_FLAG_WILL_SUICIDE)
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_MIRROR_MOVE:
        if (predictedMove != MOVE_NONE)
            return AI_CheckViability(battlerAtk, battlerDef, predictedMove, score);
        break;
    case EFFECT_ATTACK_UP:
    case EFFECT_ATTACK_UP_USER_ALLY:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ATK));
        if (IsValidDoubleBattle(battlerAtk))
            ADJUST_SCORE(IncreaseStatUpScore(BATTLE_PARTNER(battlerAtk), battlerDef, STAT_CHANGE_ATK));
        break;
    case EFFECT_ATTACK_UP_2:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ATK_2));
        break;
    case EFFECT_DEFENSE_UP:
    case EFFECT_DEFENSE_UP_3:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_DEF));
        break;
    case EFFECT_DEFENSE_UP_2:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_DEF_2));
        break;
    case EFFECT_SPEED_UP:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPEED));
        break;
    case EFFECT_SPEED_UP_2:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPEED_2));
        break;
    case EFFECT_SPECIAL_ATTACK_UP:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPATK));
        break;
    case EFFECT_SPECIAL_ATTACK_UP_2:
    case EFFECT_SPECIAL_ATTACK_UP_3:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPATK_2));
        break;
    case EFFECT_SPECIAL_DEFENSE_UP:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPDEF));
        break;
    case EFFECT_SPECIAL_DEFENSE_UP_2:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPDEF_2));
        break;
    case EFFECT_ACCURACY_UP:
    case EFFECT_ACCURACY_UP_2:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ACC));
        break;
    case EFFECT_EVASION_UP:
    case EFFECT_EVASION_UP_2:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_EVASION));
        break;
    case EFFECT_ATTACK_DOWN:
    case EFFECT_ATTACK_DOWN_2:
        if (!ShouldLowerAttack(battlerAtk, battlerDef, aiData->abilities[battlerDef]))
            ADJUST_SCORE(-2);
        if (gBattleMons[battlerDef].statStages[STAT_ATK] < DEFAULT_STAT_STAGE)
            ADJUST_SCORE(-1);
        else if (aiData->hpPercents[battlerAtk] <= 90)
            ADJUST_SCORE(-1);
        if (gBattleMons[battlerDef].statStages[STAT_ATK] > 3 && !AI_RandLessThan(50))
            ADJUST_SCORE(-2);
        else if (aiData->hpPercents[battlerDef] < 70)
            ADJUST_SCORE(-2);
        break;
    case EFFECT_DEFENSE_DOWN:
    case EFFECT_DEFENSE_DOWN_2:
        if (!ShouldLowerDefense(battlerAtk, battlerDef, aiData->abilities[battlerDef]))
            ADJUST_SCORE(-2);
        if ((aiData->hpPercents[battlerAtk] < 70 && !AI_RandLessThan(50)) || (gBattleMons[battlerDef].statStages[STAT_DEF] <= 3 && !AI_RandLessThan(50)))
            ADJUST_SCORE(-2);
        if (aiData->hpPercents[battlerDef] <= 70)
            ADJUST_SCORE(-2);
        break;
    case EFFECT_SPEED_DOWN:
    case EFFECT_SPEED_DOWN_2:
        if (AI_IsFaster(battlerAtk, battlerDef, move))
            ADJUST_SCORE(-3);
        else if (!AI_RandLessThan(70))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_SPECIAL_ATTACK_DOWN:
    case EFFECT_SPECIAL_ATTACK_DOWN_2:
        if (!ShouldLowerSpAtk(battlerAtk, battlerDef, aiData->abilities[battlerDef]))
            ADJUST_SCORE(-2);
        if (gBattleMons[battlerDef].statStages[STAT_SPATK] < DEFAULT_STAT_STAGE)
            ADJUST_SCORE(-1);
        else if (aiData->hpPercents[battlerAtk] <= 90)
            ADJUST_SCORE(-1);
        if (gBattleMons[battlerDef].statStages[STAT_SPATK] > 3 && !AI_RandLessThan(50))
            ADJUST_SCORE(-2);
        else if (aiData->hpPercents[battlerDef] < 70)
            ADJUST_SCORE(-2);
        break;
    case EFFECT_SPECIAL_DEFENSE_DOWN:
    case EFFECT_SPECIAL_DEFENSE_DOWN_2:
        if (!ShouldLowerSpDef(battlerAtk, battlerDef, aiData->abilities[battlerDef]))
            ADJUST_SCORE(-2);
        if ((aiData->hpPercents[battlerAtk] < 70 && !AI_RandLessThan(50))
          || (gBattleMons[battlerDef].statStages[STAT_SPDEF] <= 3 && !AI_RandLessThan(50)))
            ADJUST_SCORE(-2);
        if (aiData->hpPercents[battlerDef] <= 70)
            ADJUST_SCORE(-2);
        break;
    case EFFECT_ACCURACY_DOWN:
    case EFFECT_ACCURACY_DOWN_2:
        if (ShouldLowerAccuracy(battlerAtk, battlerDef, aiData->abilities[battlerDef]))
            ADJUST_SCORE(-2);
        if ((aiData->hpPercents[battlerAtk] < 70 || aiData->hpPercents[battlerDef] < 70) && AI_RandLessThan(100))
            ADJUST_SCORE(-1);
        if (gBattleMons[battlerDef].statStages[STAT_ACC] <= 4 && !AI_RandLessThan(80))
            ADJUST_SCORE(-2);
        if (gBattleMons[battlerDef].status1 & STATUS1_PSN_ANY && !AI_RandLessThan(70))
            ADJUST_SCORE(DECENT_EFFECT);
        if (gStatuses3[battlerDef] & STATUS3_LEECHSEED && !AI_RandLessThan(70))
            ADJUST_SCORE(DECENT_EFFECT);
        if (gStatuses3[battlerDef] & STATUS3_ROOTED && AI_RandLessThan(128))
            ADJUST_SCORE(WEAK_EFFECT);
        if (gBattleMons[battlerDef].status2 & STATUS2_CURSED && !AI_RandLessThan(70))
            ADJUST_SCORE(DECENT_EFFECT);
        if (aiData->hpPercents[battlerAtk] > 70 || gBattleMons[battlerDef].statStages[STAT_ACC] < DEFAULT_STAT_STAGE)
            break;
        else if (aiData->hpPercents[battlerAtk] < 40 || aiData->hpPercents[battlerDef] < 40 || !AI_RandLessThan(70))
            ADJUST_SCORE(-2);
        break;
    case EFFECT_EVASION_DOWN:
    case EFFECT_EVASION_DOWN_2:
        if (!ShouldLowerEvasion(battlerAtk, battlerDef, aiData->abilities[battlerDef]))
            ADJUST_SCORE(-2);
        if ((aiData->hpPercents[battlerAtk] < 70 || gBattleMons[battlerDef].statStages[STAT_EVASION] <= 3) && !AI_RandLessThan(50))
            ADJUST_SCORE(-2);
        if (aiData->hpPercents[battlerDef] <= 70)
            ADJUST_SCORE(-2);
        if (gBattleMons[battlerAtk].statStages[STAT_ACC] < DEFAULT_STAT_STAGE)
            ADJUST_SCORE(WEAK_EFFECT);
        if (gBattleMons[battlerDef].statStages[STAT_EVASION] < 7 || aiData->abilities[battlerAtk] == ABILITY_NO_GUARD)
            ADJUST_SCORE(-2);
        break;
    case EFFECT_SPICY_EXTRACT:
        // TODO: Make IncreaseStatDownScore function, just like IncreaseStatUpScore
        break;
    case EFFECT_BIDE:
        if (aiData->hpPercents[battlerAtk] < 90)
            ADJUST_SCORE(-2); // Should be either removed or turned into increasing score
    case EFFECT_ACUPRESSURE:
        break;
    case EFFECT_ATTACK_ACCURACY_UP: // hone claws
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ATK));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ACC));
        break;
    case EFFECT_GROWTH:
    case EFFECT_ATTACK_SPATK_UP:    // work up
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ATK));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPATK));
        break;
    case EFFECT_HAZE:
        if (AnyStatIsRaised(BATTLE_PARTNER(battlerAtk))
          || PartnerHasSameMoveEffectWithoutTarget(BATTLE_PARTNER(battlerAtk), move, aiData->partnerMove))
            break;
        score += AI_TryToClearStats(battlerAtk, battlerDef, isDoubleBattle);
        break;
    case EFFECT_ROAR:
        if ((IsSoundMove(move) && aiData->abilities[battlerDef] == ABILITY_SOUNDPROOF)
          || aiData->abilities[battlerDef] == ABILITY_SUCTION_CUPS)
            break;
        else if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
            break;
        score += AI_TryToClearStats(battlerAtk, battlerDef, isDoubleBattle);
        break;
    case EFFECT_MULTI_HIT:
    case EFFECT_TRIPLE_KICK:
    case EFFECT_POPULATION_BOMB:
        if (AI_MoveMakesContact(aiData->abilities[battlerAtk], aiData->holdEffects[battlerAtk], move)
          && aiData->abilities[battlerAtk] != ABILITY_MAGIC_GUARD
          && aiData->holdEffects[battlerDef] == HOLD_EFFECT_ROCKY_HELMET)
            ADJUST_SCORE(-2);
        break;
    case EFFECT_CONVERSION:
        if (!IS_BATTLER_OF_TYPE(battlerAtk, GetMoveType(gBattleMons[battlerAtk].moves[0])))
            ADJUST_SCORE(WEAK_EFFECT);
        break;
    case EFFECT_SWALLOW:
        if (gDisableStructs[battlerAtk].stockpileCounter == 0)
        {
            break;
        }
        else
        {
            u32 healPercent = 0;
            switch (gDisableStructs[battlerAtk].stockpileCounter)
            {
            case 1:
                healPercent = 25;
                break;
            case 2:
                healPercent = 50;
                break;
            case 3:
                healPercent = 100;
                break;
            default:
                break;
            }

            if (ShouldRecover(battlerAtk, battlerDef, move, healPercent))
                ADJUST_SCORE(DECENT_EFFECT);
        }
        break;
    case EFFECT_RESTORE_HP:
    case EFFECT_SOFTBOILED:
    case EFFECT_ROOST:
    case EFFECT_MORNING_SUN:
    case EFFECT_SYNTHESIS:
    case EFFECT_MOONLIGHT:
        if (ShouldRecover(battlerAtk, battlerDef, move, 50))
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_TOXIC:
    case EFFECT_POISON:
        IncreasePoisonScore(battlerAtk, battlerDef, move, &score);
        break;
    case EFFECT_LIGHT_SCREEN:
    case EFFECT_REFLECT:
    case EFFECT_AURORA_VEIL:
        if (ShouldSetScreen(battlerAtk, battlerDef, moveEffect))
        {
            ADJUST_SCORE(BEST_EFFECT);
            if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_LIGHT_CLAY)
                ADJUST_SCORE(DECENT_EFFECT);
        }
        break;
    case EFFECT_REST:
        if (!(CanBeSlept(battlerAtk, aiData->abilities[battlerAtk], NOT_BLOCKED_BY_SLEEP_CLAUSE)))
        {
            break;
        }
        else if (ShouldRecover(battlerAtk, battlerDef, move, 100))
        {
            if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_CURE_SLP
              || aiData->holdEffects[battlerAtk] == HOLD_EFFECT_CURE_STATUS
              || HasMoveEffect(EFFECT_SLEEP_TALK, battlerAtk)
              || HasMoveEffect(EFFECT_SNORE, battlerAtk)
              || aiData->abilities[battlerAtk] == ABILITY_SHED_SKIN
              || aiData->abilities[battlerAtk] == ABILITY_EARLY_BIRD
              || (AI_GetWeather() & B_WEATHER_RAIN && gWishFutureKnock.weatherDuration != 1 && aiData->abilities[battlerAtk] == ABILITY_HYDRATION && aiData->holdEffects[battlerAtk] != HOLD_EFFECT_UTILITY_UMBRELLA))
                ADJUST_SCORE(GOOD_EFFECT);
        }
        break;
    case EFFECT_OHKO:
        if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
            break;
        else if (gStatuses3[battlerAtk] & STATUS3_ALWAYS_HITS)
            ADJUST_SCORE(20);
        break;
    case EFFECT_MEAN_LOOK:
        if (ShouldTrap(battlerAtk, battlerDef, move))
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_FOCUS_ENERGY:
    case EFFECT_LASER_FOCUS:
        if (aiData->abilities[battlerAtk] == ABILITY_SUPER_LUCK
          || aiData->abilities[battlerAtk] == ABILITY_SNIPER
          || aiData->holdEffects[battlerAtk] == HOLD_EFFECT_SCOPE_LENS
          || HasHighCritRatioMove(battlerAtk))
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_CONFUSE:
        IncreaseConfusionScore(battlerAtk, battlerDef, move, &score);
        break;
    case EFFECT_PARALYZE:
        IncreaseParalyzeScore(battlerAtk, battlerDef, move, &score);
        break;
    case EFFECT_SUBSTITUTE:
    case EFFECT_SHED_TAIL:
        ADJUST_SCORE(IncreaseSubstituteMoveScore(battlerAtk, battlerDef, move));
        break;
    case EFFECT_MIMIC:
        if (AI_IsFaster(battlerAtk, battlerDef, move))
        {
            if (gLastMoves[battlerDef] != MOVE_NONE && gLastMoves[battlerDef] != 0xFFFF)
                return AI_CheckViability(battlerAtk, battlerDef, gLastMoves[battlerDef], score);
        }
        break;
    case EFFECT_LEECH_SEED:
        if (IS_BATTLER_OF_TYPE(battlerDef, TYPE_GRASS)
          || gStatuses3[battlerDef] & STATUS3_LEECHSEED
          || HasMoveEffect(battlerDef, EFFECT_RAPID_SPIN)
          || aiData->abilities[battlerDef] == ABILITY_LIQUID_OOZE
          || aiData->abilities[battlerDef] == ABILITY_MAGIC_GUARD)
            break;
        ADJUST_SCORE(GOOD_EFFECT);
        if (!HasDamagingMove(battlerDef) || IsBattlerTrapped(battlerDef, FALSE)
            || aiData->holdEffects[battlerAtk] == HOLD_EFFECT_BIG_ROOT)
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_DO_NOTHING:
    case EFFECT_HOLD_HANDS:
    case EFFECT_CELEBRATE:
    case EFFECT_HAPPY_HOUR:
        //todo - check z splash, z celebrate, z happy hour (lol)
        break;
    case EFFECT_TELEPORT: // Either remove or add better logic
        if (!(gBattleTypeFlags & BATTLE_TYPE_TRAINER) || GetBattlerSide(battlerAtk) != B_SIDE_PLAYER)
            break;
        //fallthrough
    case EFFECT_HIT_ESCAPE:
    case EFFECT_PARTING_SHOT:
    case EFFECT_CHILLY_RECEPTION:
        if (!IsDoubleBattle())
        {
            switch (ShouldPivot(battlerAtk, battlerDef, aiData->abilities[battlerDef], move, movesetIndex))
            {
            case DONT_PIVOT:
                ADJUST_SCORE(-10);    // technically should go in CheckBadMove, but this is easier/less computationally demanding
                break;
            case CAN_TRY_PIVOT:
                break;
            case SHOULD_PIVOT:
                ADJUST_SCORE(BEST_EFFECT);
                break;
            }
        }
        else //Double Battle
        {
            if (CountUsablePartyMons(battlerAtk) == 0)
                break; // Can't switch

            //if (switchAbility == ABILITY_INTIMIDATE && PartyHasMoveCategory(battlerDef, DAMAGE_CATEGORY_PHYSICAL))
                //ADJUST_SCORE(7);
        }
        break;
    case EFFECT_BATON_PASS:
        if ((AI_DATA->shouldSwitch & (1u << battlerAtk)) && (gBattleMons[battlerAtk].status2 & STATUS2_SUBSTITUTE
          || (gStatuses3[battlerAtk] & (STATUS3_ROOTED | STATUS3_AQUA_RING | STATUS3_MAGNET_RISE | STATUS3_POWER_TRICK))
          || AnyStatIsRaised(battlerAtk)))
            ADJUST_SCORE(BEST_EFFECT);
        break;
    case EFFECT_DISABLE:
        if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
            break;
        else if (gDisableStructs[battlerDef].disableTimer == 0
        && (gLastMoves[battlerDef] != MOVE_NONE)
        && (gLastMoves[battlerDef] != 0xFFFF)
        && (B_MENTAL_HERB < GEN_5 || aiData->holdEffects[battlerDef] != HOLD_EFFECT_MENTAL_HERB)
        && (AI_IsFaster(battlerAtk, battlerDef, move)))
        {
            if (CanTargetMoveFaintAi(gLastMoves[battlerDef], battlerDef, battlerAtk, 1))
                ADJUST_SCORE(GOOD_EFFECT); // Disable move that can kill attacker
        }
        break;
    case EFFECT_ENCORE:
        if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
            break;
        else if (gDisableStructs[battlerDef].encoreTimer == 0
        && (B_MENTAL_HERB < GEN_5 || aiData->holdEffects[battlerDef] != HOLD_EFFECT_MENTAL_HERB)
        && (gBattleMoveEffects[GetMoveEffect(gLastMoves[battlerDef])].encourageEncore))
            ADJUST_SCORE(BEST_EFFECT);
        break;
    case EFFECT_SLEEP_TALK:
    case EFFECT_SNORE:
        if (!IsWakeupTurn(battlerAtk) && gBattleMons[battlerAtk].status1 & STATUS1_SLEEP)
            ADJUST_SCORE(BEST_EFFECT);
        break;
    case EFFECT_LOCK_ON:
        if (HasMoveEffect(battlerAtk, EFFECT_OHKO))
            ADJUST_SCORE(GOOD_EFFECT);
        else if (HasMoveWithLowAccuracy(battlerAtk, battlerDef, 85, TRUE, aiData->abilities[battlerAtk], aiData->abilities[battlerDef], aiData->holdEffects[battlerAtk], aiData->holdEffects[battlerDef]))
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_DESTINY_BOND:
        if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
            break;
        else if (AI_IsFaster(battlerAtk, battlerDef, move) && CanTargetFaintAi(battlerDef, battlerAtk))
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_SPITE:
        //TODO - predicted move
        break;
    case EFFECT_WISH:
    case EFFECT_HEAL_BELL:
        if (ShouldUseWishAromatherapy(battlerAtk, battlerDef, move))
            ADJUST_SCORE(BEST_EFFECT);
        break;
    case EFFECT_CURSE:
        if (IS_BATTLER_OF_TYPE(battlerAtk, TYPE_GHOST))
        {
            if (IsBattlerTrapped(battlerDef, TRUE))
                ADJUST_SCORE(GOOD_EFFECT);
            else
                ADJUST_SCORE(WEAK_EFFECT);
        }
        else
        {
            ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ATK));
            ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_DEF));
        }
        break;
    case EFFECT_PROTECT:
        if (predictedMove == 0xFFFF)
            predictedMove = MOVE_NONE;
        switch (move)
        {
        case MOVE_QUICK_GUARD:
            if (predictedMove != MOVE_NONE && GetMovePriority(predictedMove) > 0)
                ProtectChecks(battlerAtk, battlerDef, move, predictedMove, &score);
            break;
        case MOVE_WIDE_GUARD:
            if (predictedMove != MOVE_NONE && GetBattlerMoveTargetType(battlerDef, predictedMove) & (MOVE_TARGET_FOES_AND_ALLY | MOVE_TARGET_BOTH))
            {
                ProtectChecks(battlerAtk, battlerDef, move, predictedMove, &score);
            }
            else if (isDoubleBattle && GetBattlerMoveTargetType(BATTLE_PARTNER(battlerAtk), aiData->partnerMove) & MOVE_TARGET_FOES_AND_ALLY)
            {
                if (aiData->abilities[battlerAtk] != ABILITY_TELEPATHY)
                  ProtectChecks(battlerAtk, battlerDef, move, predictedMove, &score);
            }
            break;
        case MOVE_CRAFTY_SHIELD:
            if (predictedMove != MOVE_NONE && IsBattleMoveStatus(predictedMove) && !(GetBattlerMoveTargetType(battlerDef, predictedMove) & MOVE_TARGET_USER))
                ProtectChecks(battlerAtk, battlerDef, move, predictedMove, &score);
            break;

        case MOVE_MAT_BLOCK:
            if (gDisableStructs[battlerAtk].isFirstTurn && predictedMove != MOVE_NONE
              && !IsBattleMoveStatus(predictedMove) && !(GetBattlerMoveTargetType(battlerDef, predictedMove) & MOVE_TARGET_USER))
                ProtectChecks(battlerAtk, battlerDef, move, predictedMove, &score);
            break;
        case MOVE_KINGS_SHIELD:
            if (aiData->abilities[battlerAtk] == ABILITY_STANCE_CHANGE //Special logic for Aegislash
             && gBattleMons[battlerAtk].species == SPECIES_AEGISLASH_BLADE
             && !IsBattlerIncapacitated(battlerDef, aiData->abilities[battlerDef]))
            {
                ADJUST_SCORE(GOOD_EFFECT);
                break;
            }
            //fallthrough
        default: // protect
            ProtectChecks(battlerAtk, battlerDef, move, predictedMove, &score);
            break;
        }
        break;
    case EFFECT_ENDURE:
        if (CanTargetFaintAi(battlerDef, battlerAtk))
        {
            if (gBattleMons[battlerAtk].hp > gBattleMons[battlerAtk].maxHP / 4 // Pinch berry couldn't have activated yet
             && IsPinchBerryItemEffect(aiData->holdEffects[battlerAtk]))
                ADJUST_SCORE(GOOD_EFFECT);
            else if ((gBattleMons[battlerAtk].hp > 1) // Only spam endure for Flail/Reversal if you're not at Min Health
             && (HasMoveEffect(battlerAtk, EFFECT_FLAIL) || HasMoveEffect(battlerAtk, EFFECT_ENDEAVOR)))
                ADJUST_SCORE(GOOD_EFFECT);
        }
        break;
    case EFFECT_SPIKES:
    case EFFECT_STEALTH_ROCK:
    case EFFECT_STICKY_WEB:
    case EFFECT_TOXIC_SPIKES:
        if (AI_ShouldSetUpHazards(battlerAtk, battlerDef, aiData))
        {
            if (gDisableStructs[battlerAtk].isFirstTurn)
                ADJUST_SCORE(BEST_EFFECT);
            else
                ADJUST_SCORE(DECENT_EFFECT);
        }
        break;
    case EFFECT_FORESIGHT:
        if (aiData->abilities[battlerAtk] == ABILITY_SCRAPPY || aiData->abilities[battlerAtk] == ABILITY_MINDS_EYE)
            break;
        else if (gBattleMons[battlerDef].statStages[STAT_EVASION] > DEFAULT_STAT_STAGE
         || (IS_BATTLER_OF_TYPE(battlerDef, TYPE_GHOST)
         && (HasMoveWithType(battlerAtk, TYPE_NORMAL)
         || HasMoveWithType(battlerAtk, TYPE_FIGHTING))))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_MIRACLE_EYE:
        if (gBattleMons[battlerDef].statStages[STAT_EVASION] > DEFAULT_STAT_STAGE
          || (IS_BATTLER_OF_TYPE(battlerDef, TYPE_DARK) && (HasMoveWithType(battlerAtk, TYPE_PSYCHIC))))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_PERISH_SONG:
        if (IsBattlerTrapped(battlerDef, TRUE))
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_SANDSTORM:
        if (ShouldSetSandstorm(battlerAtk, aiData->abilities[battlerAtk], aiData->holdEffects[battlerAtk]))
        {
            ADJUST_SCORE(DECENT_EFFECT);
            if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_SMOOTH_ROCK)
                ADJUST_SCORE(WEAK_EFFECT);
            if (HasMoveEffect(battlerDef, EFFECT_MORNING_SUN)
              || HasMoveEffect(battlerDef, EFFECT_SYNTHESIS)
              || HasMoveEffect(battlerDef, EFFECT_MOONLIGHT))
                ADJUST_SCORE(WEAK_EFFECT);
        }
        break;
    case EFFECT_HAIL:
        if (ShouldSetHail(battlerAtk, aiData->abilities[battlerAtk], aiData->holdEffects[battlerAtk]))
        {
            if ((HasMoveEffect(battlerAtk, EFFECT_AURORA_VEIL) || HasMoveEffect(BATTLE_PARTNER(battlerAtk), EFFECT_AURORA_VEIL))
              && ShouldSetScreen(battlerAtk, battlerDef, EFFECT_AURORA_VEIL))
                ADJUST_SCORE(GOOD_EFFECT);

            ADJUST_SCORE(DECENT_EFFECT);
            if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_ICY_ROCK)
                ADJUST_SCORE(WEAK_EFFECT);
            if (HasMoveEffect(battlerDef, EFFECT_MORNING_SUN)
              || HasMoveEffect(battlerDef, EFFECT_SYNTHESIS)
              || HasMoveEffect(battlerDef, EFFECT_MOONLIGHT))
                ADJUST_SCORE(WEAK_EFFECT);
        }
        break;
    case EFFECT_SNOWSCAPE:
        if (ShouldSetSnow(battlerAtk, aiData->abilities[battlerAtk], aiData->holdEffects[battlerAtk]))
        {
            if ((HasMoveEffect(battlerAtk, EFFECT_AURORA_VEIL) || HasMoveEffect(BATTLE_PARTNER(battlerAtk), EFFECT_AURORA_VEIL))
              && ShouldSetScreen(battlerAtk, battlerDef, EFFECT_AURORA_VEIL))
                ADJUST_SCORE(GOOD_EFFECT);

            ADJUST_SCORE(DECENT_EFFECT);
            if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_ICY_ROCK)
                ADJUST_SCORE(WEAK_EFFECT);
            if (HasMoveEffect(battlerDef, EFFECT_MORNING_SUN)
              || HasMoveEffect(battlerDef, EFFECT_SYNTHESIS)
              || HasMoveEffect(battlerDef, EFFECT_MOONLIGHT))
                ADJUST_SCORE(WEAK_EFFECT);
        }
        break;
    case EFFECT_RAIN_DANCE:
        if (ShouldSetRain(battlerAtk, aiData->abilities[battlerAtk], aiData->holdEffects[battlerAtk]))
        {
            ADJUST_SCORE(DECENT_EFFECT);
            if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_DAMP_ROCK)
                ADJUST_SCORE(WEAK_EFFECT);
            if (HasMoveEffect(battlerDef, EFFECT_MORNING_SUN)
              || HasMoveEffect(battlerDef, EFFECT_SYNTHESIS)
              || HasMoveEffect(battlerDef, EFFECT_SOLAR_BEAM)
              || HasMoveEffect(battlerDef, EFFECT_MOONLIGHT))
                ADJUST_SCORE(WEAK_EFFECT);
            if (HasMoveWithType(battlerDef, TYPE_FIRE) || HasMoveWithType(BATTLE_PARTNER(battlerDef), TYPE_FIRE))
                ADJUST_SCORE(WEAK_EFFECT);
        }
        break;
    case EFFECT_SUNNY_DAY:
        if (ShouldSetSun(battlerAtk, aiData->abilities[battlerAtk], aiData->holdEffects[battlerAtk]))
        {
            ADJUST_SCORE(DECENT_EFFECT);
            if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_HEAT_ROCK)
                ADJUST_SCORE(WEAK_EFFECT);
            if (HasMoveWithType(battlerDef, TYPE_WATER) || HasMoveWithType(BATTLE_PARTNER(battlerDef), TYPE_WATER))
                ADJUST_SCORE(WEAK_EFFECT);
            if (HasMoveEffect(battlerDef, EFFECT_THUNDER) || HasMoveEffect(BATTLE_PARTNER(battlerDef), EFFECT_THUNDER))
                ADJUST_SCORE(WEAK_EFFECT);
        }
        break;
    case EFFECT_FELL_STINGER:
        if (gBattleMons[battlerAtk].statStages[STAT_ATK] < MAX_STAT_STAGE
        && aiData->abilities[battlerAtk] != ABILITY_CONTRARY
        && CanIndexMoveFaintTarget(battlerAtk, battlerDef, movesetIndex, 0))
            ADJUST_SCORE(BEST_EFFECT);
        break;
    case EFFECT_BELLY_DRUM:
        if (!CanTargetFaintAi(battlerDef, battlerAtk)
        && gBattleMons[battlerAtk].statStages[STAT_ATK] < MAX_STAT_STAGE - 2
        && HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL)
        && aiData->abilities[battlerAtk] != ABILITY_CONTRARY)
            ADJUST_SCORE(BEST_EFFECT);
        break;
    case EFFECT_PSYCH_UP:
        score += AI_ShouldCopyStatChanges(battlerAtk, battlerDef);
        break;
    case EFFECT_SEMI_INVULNERABLE:
        if (predictedMove != MOVE_NONE && !isDoubleBattle)
        {
            u32 predictedEffect = GetMoveEffect(predictedMove);
            if ((AI_IsFaster(battlerAtk, battlerDef, move))
             && (predictedEffect == EFFECT_EXPLOSION || predictedEffect == EFFECT_PROTECT))
                ADJUST_SCORE(GOOD_EFFECT);
            else if (predictedEffect == EFFECT_SEMI_INVULNERABLE && !(gStatuses3[battlerDef] & STATUS3_SEMI_INVULNERABLE))
                ADJUST_SCORE(GOOD_EFFECT);
        }
        break;
    case EFFECT_DEFENSE_CURL:
        if (HasMoveEffect(battlerAtk, EFFECT_ROLLOUT) && !(gBattleMons[battlerAtk].status2 & STATUS2_DEFENSE_CURL))
            ADJUST_SCORE(DECENT_EFFECT);
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_DEF));
        break;
    case EFFECT_FIRST_TURN_ONLY:
        if (ShouldFakeOut(battlerAtk, battlerDef, move) && MoveHasAdditionalEffectWithChance(move, MOVE_EFFECT_FLINCH, 100))
            ADJUST_SCORE(GOOD_EFFECT);
        else if (gDisableStructs[battlerAtk].isFirstTurn && GetBestDmgMoveFromBattler(battlerAtk, battlerDef) == move)
            ADJUST_SCORE(BEST_EFFECT);
        break;
    case EFFECT_STOCKPILE:
        if (aiData->abilities[battlerAtk] == ABILITY_CONTRARY)
            break;
        if (HasMoveEffect(battlerAtk, EFFECT_SWALLOW) || HasMoveEffect(battlerAtk, EFFECT_SPIT_UP))
            ADJUST_SCORE(DECENT_EFFECT);
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_DEF));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPDEF));
        break;
    case EFFECT_SWAGGER:
    case EFFECT_FLATTER:
        if (HasMoveEffect(battlerAtk, EFFECT_FOUL_PLAY)
         || HasMoveEffect(battlerAtk, EFFECT_PSYCH_UP)
         || HasMoveEffect(battlerAtk, EFFECT_SPECTRAL_THIEF))
            ADJUST_SCORE(DECENT_EFFECT);
        if (aiData->abilities[battlerDef] == ABILITY_CONTRARY)
            ADJUST_SCORE(GOOD_EFFECT);
        IncreaseConfusionScore(battlerAtk, battlerDef, move, &score);
        break;
    case EFFECT_FURY_CUTTER:
        if (!isDoubleBattle && aiData->holdEffects[battlerAtk] == HOLD_EFFECT_METRONOME)
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_ATTRACT:
        if (!isDoubleBattle
        && (AI_IsSlower(battlerAtk, battlerDef, move))
        && BattlerWillFaintFromSecondaryDamage(battlerDef, aiData->abilities[battlerDef]))
            break; // Don't use if the attract won't have a change to activate
        if (gBattleMons[battlerDef].status1 & STATUS1_ANY
        || (gBattleMons[battlerDef].status2 & STATUS2_CONFUSION)
        || IsBattlerTrapped(battlerDef, TRUE))
            ADJUST_SCORE(GOOD_EFFECT);
        else
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_SAFEGUARD:
        if (!IsBattlerTerrainAffected(battlerAtk, STATUS_FIELD_MISTY_TERRAIN) || !IsBattlerGrounded(battlerAtk))
            ADJUST_SCORE(DECENT_EFFECT); // TODO: check if opp has status move?
        //if (CountUsablePartyMons(battlerDef) != 0)
            //ADJUST_SCORE(8);
        break;
    case EFFECT_DEFOG:
        if ((gSideStatuses[GetBattlerSide(battlerAtk)] & SIDE_STATUS_HAZARDS_ANY && CountUsablePartyMons(battlerAtk) != 0)
            || (gSideStatuses[GetBattlerSide(battlerDef)] & (SIDE_STATUS_SCREEN_ANY | SIDE_STATUS_SAFEGUARD | SIDE_STATUS_MIST)))
        {
            ADJUST_SCORE(GOOD_EFFECT);
        }
        else if (!(gSideStatuses[GetBattlerSide(battlerDef)] & SIDE_STATUS_SPIKES)) //Don't blow away hazards if you set them up
        {
            if (isDoubleBattle)
            {
                if (IsHazardMove(aiData->partnerMove) // Partner is going to set up hazards
                    && AI_IsSlower(battlerAtk, BATTLE_PARTNER(battlerAtk), move)) // Partner going first
                    break; // Don't use Defog if partner is going to set up hazards
            }
            if (ShouldLowerEvasion(battlerAtk, battlerDef, aiData->abilities[battlerDef]))
                ADJUST_SCORE(DECENT_EFFECT);
        }
        break;
    case EFFECT_TORMENT:
        break;
    case EFFECT_WILL_O_WISP:
        IncreaseBurnScore(battlerAtk, battlerDef, move, &score);
        break;
    case EFFECT_FOLLOW_ME:
        if (isDoubleBattle
          && GetMoveTarget(move) == MOVE_TARGET_USER
          && !IsBattlerIncapacitated(battlerDef, aiData->abilities[battlerDef])
          && (!IsPowderMove(move) || IsAffectedByPowder(battlerDef, aiData->abilities[battlerDef], aiData->holdEffects[battlerDef])) // Rage Powder doesn't affect powder immunities
          && IsBattlerAlive(BATTLE_PARTNER(battlerAtk)))
        {
            u32 predictedMoveOnPartner = gLastMoves[BATTLE_PARTNER(battlerAtk)];
            if (predictedMoveOnPartner != MOVE_NONE && !IsBattleMoveStatus(predictedMoveOnPartner))
                ADJUST_SCORE(GOOD_EFFECT);
        }
        break;
    case EFFECT_CHARGE:
        if (HasDamagingMoveOfType(battlerAtk, TYPE_ELECTRIC))
            ADJUST_SCORE(DECENT_EFFECT);
        if (B_CHARGE_SPDEF_RAISE >= GEN_5)
            ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPDEF));
        break;
    case EFFECT_TAUNT:
        if (IsBattleMoveStatus(predictedMove))
            ADJUST_SCORE(GOOD_EFFECT);
        else if (HasMoveWithCategory(battlerDef, DAMAGE_CATEGORY_STATUS))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_TRICK:
    case EFFECT_BESTOW:
        switch (aiData->holdEffects[battlerAtk])
        {
        case HOLD_EFFECT_CHOICE_SCARF:
            ADJUST_SCORE(DECENT_EFFECT); // assume its beneficial
            break;
        case HOLD_EFFECT_CHOICE_BAND:
            if (!HasMoveWithCategory(battlerDef, DAMAGE_CATEGORY_PHYSICAL))
                ADJUST_SCORE(DECENT_EFFECT);
            break;
        case HOLD_EFFECT_CHOICE_SPECS:
            if (!HasMoveWithCategory(battlerDef, DAMAGE_CATEGORY_SPECIAL))
                ADJUST_SCORE(DECENT_EFFECT);
            break;
        case HOLD_EFFECT_TOXIC_ORB:
            if (!ShouldPoisonSelf(battlerAtk, aiData->abilities[battlerAtk]))
                ADJUST_SCORE(DECENT_EFFECT);
            break;
        case HOLD_EFFECT_FLAME_ORB:
            if (!ShouldBurnSelf(battlerAtk, aiData->abilities[battlerAtk]) && CanBeBurned(battlerAtk, aiData->abilities[battlerDef]))
                ADJUST_SCORE(DECENT_EFFECT);
            break;
        case HOLD_EFFECT_BLACK_SLUDGE:
            if (!IS_BATTLER_OF_TYPE(battlerDef, TYPE_POISON) && aiData->abilities[battlerDef] != ABILITY_MAGIC_GUARD)
                ADJUST_SCORE(DECENT_EFFECT);
            break;
        case HOLD_EFFECT_IRON_BALL:
            if (!HasMoveEffect(battlerDef, EFFECT_FLING) || !IsBattlerGrounded(battlerDef))
                ADJUST_SCORE(DECENT_EFFECT);
            break;
        case HOLD_EFFECT_LAGGING_TAIL:
        case HOLD_EFFECT_STICKY_BARB:
            ADJUST_SCORE(DECENT_EFFECT);
            break;
        case HOLD_EFFECT_UTILITY_UMBRELLA:
            if (aiData->abilities[battlerAtk] != ABILITY_SOLAR_POWER && aiData->abilities[battlerAtk] != ABILITY_DRY_SKIN)
            {
                switch (aiData->abilities[battlerDef])
                {
                case ABILITY_SWIFT_SWIM:
                    if (AI_GetWeather() & B_WEATHER_RAIN)
                        ADJUST_SCORE(DECENT_EFFECT); // Slow 'em down
                    break;
                case ABILITY_CHLOROPHYLL:
                case ABILITY_FLOWER_GIFT:
                    if (AI_GetWeather() & B_WEATHER_SUN)
                        ADJUST_SCORE(DECENT_EFFECT); // Slow 'em down
                    break;
                }
            }
            break;
        case HOLD_EFFECT_EJECT_BUTTON:
            //if (!IsRaidBattle() && GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX && gNewBS->dynamaxData.timer[battlerDef] > 1 &&
            if (HasDamagingMove(battlerAtk)
             || (isDoubleBattle && IsBattlerAlive(BATTLE_PARTNER(battlerAtk)) && HasDamagingMove(BATTLE_PARTNER(battlerAtk))))
                ADJUST_SCORE(DECENT_EFFECT); // Force 'em out next turn
            break;
        default:
            if (GetMoveEffect(move) != EFFECT_BESTOW && aiData->items[battlerAtk] == ITEM_NONE && aiData->items[battlerDef] != ITEM_NONE)
            {
                switch (aiData->holdEffects[battlerDef])
                {
                case HOLD_EFFECT_CHOICE_BAND:
                    break;
                case HOLD_EFFECT_TOXIC_ORB:
                    if (ShouldPoisonSelf(battlerAtk, aiData->abilities[battlerAtk]))
                        ADJUST_SCORE(DECENT_EFFECT);
                    break;
                case HOLD_EFFECT_FLAME_ORB:
                    if (ShouldBurnSelf(battlerAtk, aiData->abilities[battlerAtk]))
                        ADJUST_SCORE(DECENT_EFFECT);
                    break;
                case HOLD_EFFECT_BLACK_SLUDGE:
                    if (IS_BATTLER_OF_TYPE(battlerAtk, TYPE_POISON) || aiData->abilities[battlerAtk] == ABILITY_MAGIC_GUARD)
                        ADJUST_SCORE(DECENT_EFFECT);
                    break;
                case HOLD_EFFECT_IRON_BALL:
                    if (HasMoveEffect(battlerAtk, EFFECT_FLING))
                        ADJUST_SCORE(DECENT_EFFECT);
                    break;
                case HOLD_EFFECT_LAGGING_TAIL:
                case HOLD_EFFECT_STICKY_BARB:
                    break;
                default:
                    ADJUST_SCORE(WEAK_EFFECT);    //other hold effects generally universally good
                    break;
                }
            }
        }
        break;
    case EFFECT_CORROSIVE_GAS:
        if (CanKnockOffItem(battlerDef, aiData->items[battlerDef]))
        {
            switch (aiData->holdEffects[battlerDef])
            {
            case HOLD_EFFECT_IRON_BALL:
                if (HasMoveEffect(battlerDef, EFFECT_FLING))
                    ADJUST_SCORE(DECENT_EFFECT);
                break;
            case HOLD_EFFECT_LAGGING_TAIL:
            case HOLD_EFFECT_STICKY_BARB:
                break;
            default:
                ADJUST_SCORE(DECENT_EFFECT);
                break;
            }
        }
        break;
    case EFFECT_ROLE_PLAY:
    case EFFECT_DOODLE:
        if (IsAbilityOfRating(aiData->abilities[battlerDef], 5))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_INGRAIN:
        ADJUST_SCORE(WEAK_EFFECT);
        if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_BIG_ROOT)
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_MAGIC_COAT:
        if (IsBattleMoveStatus(predictedMove) && GetBattlerMoveTargetType(battlerDef, predictedMove) & (MOVE_TARGET_SELECTED | MOVE_TARGET_OPPONENTS_FIELD | MOVE_TARGET_BOTH))
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_RECYCLE:
        if (GetUsedHeldItem(battlerAtk) != ITEM_NONE)
            ADJUST_SCORE(WEAK_EFFECT);
        if (IsRecycleEncouragedItem(GetUsedHeldItem(battlerAtk)))
            ADJUST_SCORE(WEAK_EFFECT);
        if (aiData->abilities[battlerAtk] == ABILITY_RIPEN)
        {
            u32 item = GetUsedHeldItem(battlerAtk);
            u32 toHeal = (ItemId_GetHoldEffectParam(item) == 10) ? 10 : gBattleMons[battlerAtk].maxHP / ItemId_GetHoldEffectParam(item);

            if (IsStatBoostingBerry(item) && aiData->hpPercents[battlerAtk] > 60)
                ADJUST_SCORE(WEAK_EFFECT);
            else if (ShouldRestoreHpBerry(battlerAtk, item) && !CanAIFaintTarget(battlerAtk, battlerDef, 0)
              && ((GetWhichBattlerFasterOrTies(battlerAtk, battlerDef, TRUE) == 1 && CanTargetFaintAiWithMod(battlerDef, battlerAtk, 0, 0))
               || !CanTargetFaintAiWithMod(battlerDef, battlerAtk, toHeal, 0)))
                ADJUST_SCORE(WEAK_EFFECT);    // Recycle healing berry if we can't otherwise faint the target and the target wont kill us after we activate the berry
        }
        break;
    case EFFECT_RAGING_BULL:
    case EFFECT_BRICK_BREAK:
        if (gSideStatuses[GetBattlerSide(battlerDef)] & SIDE_STATUS_REFLECT)
            ADJUST_SCORE(DECENT_EFFECT);
        if (gSideStatuses[GetBattlerSide(battlerDef)] & SIDE_STATUS_LIGHTSCREEN)
            ADJUST_SCORE(DECENT_EFFECT);
        if (gSideStatuses[GetBattlerSide(battlerDef)] & SIDE_STATUS_AURORA_VEIL)
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_SKILL_SWAP:
        if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
            break;
        else if (gAbilitiesInfo[aiData->abilities[battlerDef]].aiRating > gAbilitiesInfo[aiData->abilities[battlerAtk]].aiRating)
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_WORRY_SEED:
    case EFFECT_GASTRO_ACID:
    case EFFECT_SIMPLE_BEAM:
        if (IsAbilityOfRating(aiData->abilities[battlerDef], 5))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_ENTRAINMENT:
        if (GetActiveGimmick(battlerDef) == GIMMICK_DYNAMAX)
            break;
        if (aiData->abilities[battlerDef] != aiData->abilities[battlerAtk] && !(gStatuses3[battlerDef] & STATUS3_GASTRO_ACID))
        {
            if (gAbilitiesInfo[aiData->abilities[battlerAtk]].aiRating <= 0)
                ADJUST_SCORE(DECENT_EFFECT);
            else if (IsAbilityOfRating(aiData->abilities[battlerDef], 5) && gAbilitiesInfo[aiData->abilities[battlerAtk]].aiRating <= 3)
                ADJUST_SCORE(WEAK_EFFECT);
        }
        break;
    case EFFECT_IMPRISON:
        if (predictedMove != MOVE_NONE && HasMove(battlerAtk, predictedMove))
            ADJUST_SCORE(DECENT_EFFECT);
        else if (gDisableStructs[battlerAtk].isFirstTurn == 0)
            ADJUST_SCORE(WEAK_EFFECT);
        break;
    case EFFECT_REFRESH:
        if (gBattleMons[battlerAtk].status1 & STATUS1_ANY)
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_TAKE_HEART:
        if (gBattleMons[battlerAtk].status1 & STATUS1_ANY
         || BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPATK)
         || BattlerStatCanRise(battlerAtk, aiData->abilities[battlerAtk], STAT_SPDEF))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_PSYCHO_SHIFT:
        if (gBattleMons[battlerAtk].status1 & STATUS1_PSN_ANY)
            IncreasePoisonScore(battlerAtk, battlerDef, move, &score);
        else if (gBattleMons[battlerAtk].status1 & STATUS1_BURN)
            IncreaseBurnScore(battlerAtk, battlerDef, move, &score);
        else if (gBattleMons[battlerAtk].status1 & STATUS1_PARALYSIS)
            IncreaseParalyzeScore(battlerAtk, battlerDef, move, &score);
        else if (gBattleMons[battlerAtk].status1 & STATUS1_SLEEP)
            IncreaseSleepScore(battlerAtk, battlerDef, move, &score);
        else if (gBattleMons[battlerAtk].status1 & STATUS1_FROSTBITE)
            IncreaseFrostbiteScore(battlerAtk, battlerDef, move, &score);
        break;
    case EFFECT_GRUDGE:
        break;
    case EFFECT_SNATCH:
        if (predictedMove != MOVE_NONE && MoveCanBeSnatched(predictedMove))
            ADJUST_SCORE(GOOD_EFFECT); // Steal move
        break;
    case EFFECT_MUD_SPORT:
        if (!HasMoveWithType(battlerAtk, TYPE_ELECTRIC) && HasMoveWithType(battlerDef, TYPE_ELECTRIC))
            ADJUST_SCORE(WEAK_EFFECT);
        break;
    case EFFECT_WATER_SPORT:
        if (!HasMoveWithType(battlerAtk, TYPE_FIRE) && (HasMoveWithType(battlerDef, TYPE_FIRE)))
            ADJUST_SCORE(WEAK_EFFECT);
        break;
    case EFFECT_TICKLE:
        if (gBattleMons[battlerDef].statStages[STAT_DEF] > 4 && HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL)
         && aiData->abilities[battlerDef] != ABILITY_CONTRARY && ShouldLowerDefense(battlerAtk, battlerDef, aiData->abilities[battlerDef]))
            ADJUST_SCORE(DECENT_EFFECT);
        else if (ShouldLowerAttack(battlerAtk, battlerDef, aiData->abilities[battlerDef]))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_COSMIC_POWER:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_DEF));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPDEF));
        break;
    case EFFECT_BULK_UP:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ATK));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_DEF));
        break;
    case EFFECT_CALM_MIND:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPATK));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPDEF));
        break;
    case EFFECT_GEOMANCY:
        if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_POWER_HERB)
            ADJUST_SCORE(GOOD_EFFECT);
    case EFFECT_QUIVER_DANCE:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPEED));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPATK));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPDEF));
        break;
    case EFFECT_VICTORY_DANCE:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPEED));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ATK));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_DEF));
        break;
    case EFFECT_SHELL_SMASH:
        if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_RESTORE_STATS)
            ADJUST_SCORE(WEAK_EFFECT);

        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPEED));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPATK));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ATK));
        break;
    case EFFECT_TIDY_UP:
        IncreaseTidyUpScore(battlerAtk, battlerDef, move, &score);
    case EFFECT_DRAGON_DANCE:
    case EFFECT_SHIFT_GEAR:
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPEED));
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ATK));
        break;
    case EFFECT_GUARD_SWAP:
        if (gBattleMons[battlerDef].statStages[STAT_DEF] > gBattleMons[battlerAtk].statStages[STAT_DEF]
          && gBattleMons[battlerDef].statStages[STAT_SPDEF] >= gBattleMons[battlerAtk].statStages[STAT_SPDEF])
            ADJUST_SCORE(DECENT_EFFECT);
        else if (gBattleMons[battlerDef].statStages[STAT_SPDEF] > gBattleMons[battlerAtk].statStages[STAT_SPDEF]
          && gBattleMons[battlerDef].statStages[STAT_DEF] >= gBattleMons[battlerAtk].statStages[STAT_DEF])
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_POWER_SWAP:
        if (gBattleMons[battlerDef].statStages[STAT_ATK] > gBattleMons[battlerAtk].statStages[STAT_ATK]
          && gBattleMons[battlerDef].statStages[STAT_SPATK] >= gBattleMons[battlerAtk].statStages[STAT_SPATK])
            ADJUST_SCORE(DECENT_EFFECT);
        else if (gBattleMons[battlerDef].statStages[STAT_SPATK] > gBattleMons[battlerAtk].statStages[STAT_SPATK]
          && gBattleMons[battlerDef].statStages[STAT_ATK] >= gBattleMons[battlerAtk].statStages[STAT_ATK])
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_POWER_TRICK:
        if (!(gStatuses3[battlerAtk] & STATUS3_POWER_TRICK)
        && gBattleMons[battlerAtk].defense > gBattleMons[battlerAtk].attack
        && HasMoveWithCategory(battlerAtk, DAMAGE_CATEGORY_PHYSICAL))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_HEART_SWAP:
        {
            bool32 hasHigherStat = FALSE;
            //Only use if all target stats are >= attacker stats to prevent infinite loop
            for (i = STAT_ATK; i < NUM_BATTLE_STATS; i++)
            {
                if (gBattleMons[battlerDef].statStages[i] < gBattleMons[battlerAtk].statStages[i])
                    break;
                if (gBattleMons[battlerDef].statStages[i] > gBattleMons[battlerAtk].statStages[i])
                    hasHigherStat = TRUE;
            }
            if (hasHigherStat && i == NUM_BATTLE_STATS)
                ADJUST_SCORE(DECENT_EFFECT);
        }
        break;
    case EFFECT_SPEED_SWAP:
        if (gBattleMons[battlerDef].speed > gBattleMons[battlerAtk].speed)
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_GUARD_SPLIT:
        {
            u32 newDefense = (gBattleMons[battlerAtk].defense + gBattleMons[battlerDef].defense) / 2;
            u32 newSpDef = (gBattleMons[battlerAtk].spDefense + gBattleMons[battlerDef].spDefense) / 2;

            if ((newDefense > gBattleMons[battlerAtk].defense && newSpDef >= gBattleMons[battlerAtk].spDefense)
            || (newSpDef > gBattleMons[battlerAtk].spDefense && newDefense >= gBattleMons[battlerAtk].defense))
                ADJUST_SCORE(DECENT_EFFECT);
        }
        break;
    case EFFECT_POWER_SPLIT:
        {
            u32 newAttack = (gBattleMons[battlerAtk].attack + gBattleMons[battlerDef].attack) / 2;
            u32 newSpAtk = (gBattleMons[battlerAtk].spAttack + gBattleMons[battlerDef].spAttack) / 2;

            if ((newAttack > gBattleMons[battlerAtk].attack && newSpAtk >= gBattleMons[battlerAtk].spAttack)
            || (newSpAtk > gBattleMons[battlerAtk].spAttack && newAttack >= gBattleMons[battlerAtk].attack))
                ADJUST_SCORE(DECENT_EFFECT);
        }
        break;
    case EFFECT_ELECTRIC_TERRAIN:
    case EFFECT_MISTY_TERRAIN:
        if (gStatuses3[battlerAtk] & STATUS3_YAWN && IsBattlerGrounded(battlerAtk))
            ADJUST_SCORE(BEST_EFFECT);
    case EFFECT_GRASSY_TERRAIN:
    case EFFECT_PSYCHIC_TERRAIN:
        ADJUST_SCORE(GOOD_EFFECT);
        if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_TERRAIN_EXTENDER)
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_PLEDGE:
        if (isDoubleBattle && HasMoveEffect(BATTLE_PARTNER(battlerAtk), EFFECT_PLEDGE))
            ADJUST_SCORE(GOOD_EFFECT); // Partner might use pledge move
        break;
    case EFFECT_TRICK_ROOM:
        if (!(AI_THINKING_STRUCT->aiFlags[battlerAtk] & AI_FLAG_POWERFUL_STATUS))
        {
            if (!(gFieldStatuses & STATUS_FIELD_TRICK_ROOM) && GetBattlerSideSpeedAverage(battlerAtk) < GetBattlerSideSpeedAverage(battlerDef))
                ADJUST_SCORE(GOOD_EFFECT);
            else if ((gFieldStatuses & STATUS_FIELD_TRICK_ROOM) && GetBattlerSideSpeedAverage(battlerAtk) >= GetBattlerSideSpeedAverage(battlerDef))
                ADJUST_SCORE(GOOD_EFFECT);
        }
        break;
    case EFFECT_MAGIC_ROOM:
        ADJUST_SCORE(WEAK_EFFECT);
        if (aiData->holdEffects[battlerAtk] == HOLD_EFFECT_NONE && aiData->holdEffects[battlerDef] != HOLD_EFFECT_NONE)
            ADJUST_SCORE(WEAK_EFFECT);
        if (isDoubleBattle && aiData->holdEffects[BATTLE_PARTNER(battlerAtk)] == HOLD_EFFECT_NONE && aiData->holdEffects[BATTLE_PARTNER(battlerDef)] != HOLD_EFFECT_NONE)
            ADJUST_SCORE(WEAK_EFFECT);
        break;
    case EFFECT_WONDER_ROOM:
        if ((HasMoveWithCategory(battlerDef, DAMAGE_CATEGORY_PHYSICAL) && gBattleMons[battlerAtk].defense < gBattleMons[battlerAtk].spDefense)
          || (HasMoveWithCategory(battlerDef, DAMAGE_CATEGORY_SPECIAL) && gBattleMons[battlerAtk].spDefense < gBattleMons[battlerAtk].defense))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_GRAVITY:
        if (!(gFieldStatuses & STATUS_FIELD_GRAVITY))
        {
            if (HasSleepMoveWithLowAccuracy(battlerAtk, battlerDef)) // Has Gravity for a move like Hypnosis
                IncreaseSleepScore(battlerAtk, battlerDef, move, &score);
            if (HasMoveWithLowAccuracy(battlerAtk, battlerDef, 90, FALSE, aiData->abilities[battlerAtk], aiData->abilities[battlerDef], aiData->holdEffects[battlerAtk], aiData->holdEffects[battlerDef]))
                ADJUST_SCORE(DECENT_EFFECT);
        }
        break;
    case EFFECT_ION_DELUGE:
        if ((aiData->abilities[battlerAtk] == ABILITY_VOLT_ABSORB
          || aiData->abilities[battlerAtk] == ABILITY_MOTOR_DRIVE
          || (B_REDIRECT_ABILITY_IMMUNITY >= GEN_5 && aiData->abilities[battlerAtk] == ABILITY_LIGHTNING_ROD))
          && predictedType == TYPE_NORMAL)
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_FLING:
        /* TODO
        switch (gFlingTable[aiData->items[battlerAtk]].effect)
        {
        case MOVE_EFFECT_BURN:
            IncreaseBurnScore(battlerAtk, battlerDef, move, &score);
            break;
        case MOVE_EFFECT_FLINCH:
            score += ShouldTryToFlinch(battlerAtk, battlerDef, aiData->abilities[battlerAtk], aiData->abilities[battlerDef], move);
            break;
        case MOVE_EFFECT_PARALYSIS:
            IncreaseParalyzeScore(battlerAtk, battlerDef, move, &score);
            break;
        case MOVE_EFFECT_POISON:
        case MOVE_EFFECT_TOXIC:
            IncreasePoisonScore(battlerAtk, battlerDef, move, &score);
            break;
        case MOVE_EFFECT_FREEZE:
            if (AI_CanFreeze(battlerAtk, battlerDef))
                ADJUST_SCORE(GOOD_EFFECT);
            break;
        }*/
        break;
    case EFFECT_EMBARGO:
        if (aiData->holdEffects[battlerDef] != HOLD_EFFECT_NONE)
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_POWDER:
        if (predictedMove != MOVE_NONE && !IsBattleMoveStatus(predictedMove) && predictedType == TYPE_FIRE)
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_TELEKINESIS:
        if (HasMoveWithLowAccuracy(battlerAtk, battlerDef, 90, FALSE, aiData->abilities[battlerAtk], aiData->abilities[battlerDef], aiData->holdEffects[battlerAtk], aiData->holdEffects[battlerDef])
          || !IsBattlerGrounded(battlerDef))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_HEAL_BLOCK:
        if (AI_IsFaster(battlerAtk, battlerDef, move) && predictedMove != MOVE_NONE && IsHealingMove(predictedMove))
            ADJUST_SCORE(DECENT_EFFECT); // Try to cancel healing move
        else if (HasHealingEffect(battlerDef) || aiData->holdEffects[battlerDef] == HOLD_EFFECT_LEFTOVERS
          || (aiData->holdEffects[battlerDef] == HOLD_EFFECT_BLACK_SLUDGE && IS_BATTLER_OF_TYPE(battlerDef, TYPE_POISON)))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_SOAK:
        if (HasMoveWithType(battlerAtk, TYPE_ELECTRIC) || HasMoveWithType(battlerAtk, TYPE_GRASS) || (HasMoveEffect(battlerAtk, EFFECT_SUPER_EFFECTIVE_ON_ARG) && GetMoveArgType(move) == TYPE_WATER) )
            ADJUST_SCORE(DECENT_EFFECT); // Get some super effective moves
        break;
    case EFFECT_THIRD_TYPE:
        if (aiData->abilities[battlerDef] == ABILITY_WONDER_GUARD)
            ADJUST_SCORE(DECENT_EFFECT); // Give target more weaknesses
        break;
    case EFFECT_ELECTRIFY:
        if (predictedMove != MOVE_NONE
         && (aiData->abilities[battlerAtk] == ABILITY_VOLT_ABSORB
          || aiData->abilities[battlerAtk] == ABILITY_MOTOR_DRIVE
          || (B_REDIRECT_ABILITY_IMMUNITY >= GEN_5 && aiData->abilities[battlerAtk] == ABILITY_LIGHTNING_ROD)))
        {
            ADJUST_SCORE(DECENT_EFFECT);
        }
        break;
    case EFFECT_TOPSY_TURVY:
        if (CountPositiveStatStages(battlerDef) > CountNegativeStatStages(battlerDef))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_FAIRY_LOCK:
        if (ShouldTrap(battlerAtk, battlerDef, move))
            ADJUST_SCORE(BEST_EFFECT);
        break;
    case EFFECT_QUASH:
        if (isDoubleBattle && AI_IsSlower(BATTLE_PARTNER(battlerAtk), battlerDef, aiData->partnerMove))
            ADJUST_SCORE(DECENT_EFFECT); // Attacker partner wouldn't go before target
        break;
    case EFFECT_TAILWIND:
        if (GetBattlerSideSpeedAverage(battlerAtk) < GetBattlerSideSpeedAverage(battlerDef))
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_LUCKY_CHANT:
        if (!isDoubleBattle && CountUsablePartyMons(battlerDef) > 0)
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_MAGNET_RISE:
        if (IsBattlerGrounded(battlerAtk) && HasDamagingMoveOfType(battlerDef, TYPE_ELECTRIC)
          && !(effectiveness == UQ_4_12(0.0))) // Doesn't resist ground move
        {
            if (AI_IsFaster(battlerAtk, battlerDef, move)) // Attacker goes first
           {
                if (predictedType == TYPE_GROUND)
                    ADJUST_SCORE(GOOD_EFFECT); // Cause the enemy's move to fail
                break;
            }
            else // Opponent Goes First
            {
                if (HasDamagingMoveOfType(battlerDef, TYPE_GROUND))
                    ADJUST_SCORE(DECENT_EFFECT);
                break;
            }
        }
        break;
    case EFFECT_CAMOUFLAGE:
        if (predictedMove != MOVE_NONE && AI_IsFaster(battlerAtk, battlerDef, move) // Attacker goes first
         && !IsBattleMoveStatus(move) && effectiveness != UQ_4_12(0.0))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_TOXIC_THREAD:
        IncreasePoisonScore(battlerAtk, battlerDef, move, &score);
        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_SPEED));
        break;
    case EFFECT_COUNTER:
        if ((!IsBattlerIncapacitated(battlerDef, aiData->abilities[battlerDef]) && predictedMove != MOVE_NONE)
         && (GetNoOfHitsToKOBattler(battlerDef, battlerAtk, predictedMoveSlot) >= 2)
         && (GetBattleMoveCategory(predictedMove) == DAMAGE_CATEGORY_PHYSICAL))
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_MIRROR_COAT:
        if ((!IsBattlerIncapacitated(battlerDef, aiData->abilities[battlerDef]) && predictedMove != MOVE_NONE)
         && (GetNoOfHitsToKOBattler(battlerDef, battlerAtk, predictedMoveSlot) >= 2)
         && (GetBattleMoveCategory(predictedMove) == DAMAGE_CATEGORY_SPECIAL))
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_SHORE_UP:
        if ((AI_GetWeather() & B_WEATHER_SANDSTORM) && ShouldRecover(battlerAtk, battlerDef, move, 67))
            ADJUST_SCORE(DECENT_EFFECT);
        else if (ShouldRecover(battlerAtk, battlerDef, move, 50))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_ENDEAVOR:
        if (AI_IsSlower(battlerAtk, battlerDef, move) && !CanTargetFaintAi(battlerDef, battlerAtk))
            ADJUST_SCORE(DECENT_EFFECT);
        break;
    case EFFECT_REVIVAL_BLESSING:
        if (GetFirstFaintedPartyIndex(battlerAtk) != PARTY_SIZE)
        {
            ADJUST_SCORE(DECENT_EFFECT);
            if (AI_DATA->shouldSwitch & (1u << battlerAtk)) // Bad matchup
                ADJUST_SCORE(WEAK_EFFECT);
            if (AI_DATA->mostSuitableMonId[battlerAtk] != PARTY_SIZE) // Good mon to send in after
                ADJUST_SCORE(WEAK_EFFECT);
        }
        break;
    //case EFFECT_EXTREME_EVOBOOST: // TODO
        //break;
    //case EFFECT_CLANGOROUS_SOUL:  // TODO
        //break;
    //case EFFECT_NO_RETREAT:       // TODO
        //break;
    //case EFFECT_SKY_DROP
        //break;
    case EFFECT_JUNGLE_HEALING:
        if (ShouldRecover(battlerAtk, battlerDef, move, 25)
         || ShouldRecover(BATTLE_PARTNER(battlerAtk), battlerDef, move, 25)
         || gBattleMons[battlerAtk].status1 & STATUS1_ANY
         || gBattleMons[BATTLE_PARTNER(battlerAtk)].status1 & STATUS1_ANY)
            ADJUST_SCORE(GOOD_EFFECT);
        break;
    case EFFECT_RAPID_SPIN:
        if ((gSideStatuses[GetBattlerSide(battlerAtk)] & SIDE_STATUS_HAZARDS_ANY && CountUsablePartyMons(battlerAtk) != 0)
         || (gStatuses3[battlerAtk] & STATUS3_LEECHSEED || gBattleMons[battlerAtk].status2 & STATUS2_WRAPPED))
            ADJUST_SCORE(GOOD_EFFECT);
    case EFFECT_SPECTRAL_THIEF:
        ADJUST_SCORE(AI_ShouldCopyStatChanges(battlerAtk, battlerDef));
        break;
    } // move effect checks
    // DebugPrintf("Before additional effects score: %d", score);

    u32 additionalEffectCount = GetMoveAdditionalEffectCount(move);
    // DebugPrintf("additionalEffectCount: %d", additionalEffectCount);
    // check move additional effects that are likely to happen
    for (i = 0; i < additionalEffectCount; i++)
    {
        const struct AdditionalEffect *additionalEffect = GetMoveAdditionalEffectById(move, i);
        // // Only consider effects with a guaranteed chance to happen
        // if (!MoveEffectIsGuaranteed(battlerAtk, aiData->abilities[battlerAtk], additionalEffect))
        //     continue;
        if (MoveEffectIsGuaranteed(battlerAtk, aiData->abilities[battlerAtk], additionalEffect))
        {
            if (additionalEffect->self)
            {
                if (IsSelfStatLoweringEffect(additionalEffect->moveEffect))
                    ADJUST_SCORE(-2);
                u32 StageStatId;

                if (aiData->abilities[battlerAtk] != ABILITY_CONTRARY)
                {
                    switch (additionalEffect->moveEffect)
                    {
                    case MOVE_EFFECT_ATK_PLUS_1:
                    case MOVE_EFFECT_DEF_PLUS_1:
                    case MOVE_EFFECT_SPD_PLUS_1:
                    case MOVE_EFFECT_SP_ATK_PLUS_1:
                    case MOVE_EFFECT_SP_DEF_PLUS_1:
                        StageStatId = STAT_CHANGE_ATK + additionalEffect->moveEffect - MOVE_EFFECT_ATK_PLUS_1;
                        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, StageStatId));
                        break;
                    case MOVE_EFFECT_ATK_PLUS_2:
                    case MOVE_EFFECT_DEF_PLUS_2:
                    case MOVE_EFFECT_SPD_PLUS_2:
                    case MOVE_EFFECT_SP_ATK_PLUS_2:
                    case MOVE_EFFECT_SP_DEF_PLUS_2:
                        StageStatId = STAT_CHANGE_ATK_2 + additionalEffect->moveEffect - MOVE_EFFECT_ATK_PLUS_1;
                        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, StageStatId));
                        break;
                    case MOVE_EFFECT_ACC_PLUS_1:
                    case MOVE_EFFECT_ACC_PLUS_2:
                        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_ACC));
                        break;
                    case MOVE_EFFECT_EVS_PLUS_1:
                    case MOVE_EFFECT_EVS_PLUS_2:
                        ADJUST_SCORE(IncreaseStatUpScore(battlerAtk, battlerDef, STAT_CHANGE_EVASION));
                        break;
                    }
                }
                else
                {
                    switch (additionalEffect->moveEffect)
                    {
                    case MOVE_EFFECT_ATK_MINUS_1:
                    case MOVE_EFFECT_DEF_MINUS_1:
                    case MOVE_EFFECT_SPD_MINUS_1:
                    case MOVE_EFFECT_SP_ATK_MINUS_1:
                    case MOVE_EFFECT_SP_DEF_MINUS_1:
                        StageStatId = STAT_CHANGE_ATK + additionalEffect->moveEffect - MOVE_EFFECT_ATK_MINUS_1;
                        ADJUST_SCORE(IncreaseStatUpScoreContrary(battlerAtk, battlerDef, StageStatId));
                        break;
                    case MOVE_EFFECT_ATK_MINUS_2:
                    case MOVE_EFFECT_DEF_MINUS_2:
                    case MOVE_EFFECT_SPD_MINUS_2:
                    case MOVE_EFFECT_SP_ATK_MINUS_2:
                    case MOVE_EFFECT_SP_DEF_MINUS_2:
                        StageStatId = STAT_CHANGE_ATK + additionalEffect->moveEffect - MOVE_EFFECT_ATK_MINUS_2;
                        ADJUST_SCORE(IncreaseStatUpScoreContrary(battlerAtk, battlerDef, StageStatId));
                        break;
                    case MOVE_EFFECT_ACC_MINUS_1:
                    case MOVE_EFFECT_ACC_MINUS_2:
                        ADJUST_SCORE(IncreaseStatUpScoreContrary(battlerAtk, battlerDef, STAT_CHANGE_ACC));
                        break;
                    case MOVE_EFFECT_EVS_MINUS_1:
                    case MOVE_EFFECT_EVS_MINUS_2:
                        ADJUST_SCORE(IncreaseStatUpScoreContrary(battlerAtk, battlerDef, STAT_CHANGE_EVASION));
                        break;
                    case MOVE_EFFECT_DEF_SPDEF_DOWN:
                        ADJUST_SCORE(IncreaseStatUpScoreContrary(battlerAtk, battlerDef, STAT_CHANGE_DEF));
                        ADJUST_SCORE(IncreaseStatUpScoreContrary(battlerAtk, battlerDef, STAT_CHANGE_SPDEF));
                        break;
                    case MOVE_EFFECT_ATK_DEF_DOWN:
                        ADJUST_SCORE(IncreaseStatUpScoreContrary(battlerAtk, battlerDef, STAT_CHANGE_ATK));
                        ADJUST_SCORE(IncreaseStatUpScoreContrary(battlerAtk, battlerDef, STAT_CHANGE_DEF));
                        break;
                    case MOVE_EFFECT_V_CREATE:
                        ADJUST_SCORE(IncreaseStatUpScoreContrary(battlerAtk, battlerDef, STAT_CHANGE_DEF));
                        ADJUST_SCORE(IncreaseStatUpScoreContrary(battlerAtk, battlerDef, STAT_CHANGE_SPEED));
                        ADJUST_SCORE(IncreaseStatUpScoreContrary(battlerAtk, battlerDef, STAT_CHANGE_SPDEF));
                        break;
                    }
                }
            }
            else // consider move effects that hinder the target
            {
                // DebugPrintf("ELSE Current Score: %d", score);
                switch (additionalEffect->moveEffect)
                {
                    case MOVE_EFFECT_FLINCH:
                        score += ShouldTryToFlinch(battlerAtk, battlerDef, aiData->abilities[battlerAtk], aiData->abilities[battlerDef], move);
                        break;
                    case MOVE_EFFECT_SPD_MINUS_1:
                    case MOVE_EFFECT_SPD_MINUS_2:
                        if (ShouldLowerSpeed(battlerAtk, battlerDef, aiData->abilities[battlerDef]))
                            ADJUST_SCORE(DECENT_EFFECT);
                    case MOVE_EFFECT_ATK_MINUS_1:
                    case MOVE_EFFECT_DEF_MINUS_1:
                    case MOVE_EFFECT_SP_ATK_MINUS_1:
                    case MOVE_EFFECT_SP_DEF_MINUS_1:
                    case MOVE_EFFECT_ACC_MINUS_1:
                    case MOVE_EFFECT_EVS_MINUS_1:
                        if (aiData->abilities[battlerDef] != ABILITY_CONTRARY)
                            ADJUST_SCORE(DECENT_EFFECT);
                        break;
                    case MOVE_EFFECT_ATK_MINUS_2:
                    case MOVE_EFFECT_DEF_MINUS_2:
                    case MOVE_EFFECT_SP_ATK_MINUS_2:
                    case MOVE_EFFECT_SP_DEF_MINUS_2:
                    case MOVE_EFFECT_ACC_MINUS_2:
                    case MOVE_EFFECT_EVS_MINUS_2:
                        if (aiData->abilities[battlerDef] != ABILITY_CONTRARY)
                            ADJUST_SCORE(DECENT_EFFECT);
                        break;
                    case MOVE_EFFECT_POISON:
                        IncreasePoisonScore(battlerAtk, battlerDef, move, &score);
                        break;
                    case MOVE_EFFECT_CLEAR_SMOG:
                        score += AI_TryToClearStats(battlerAtk, battlerDef, FALSE);
                        break;
                    case MOVE_EFFECT_BUG_BITE:   // And pluck
                        if (gBattleMons[battlerDef].status2 & STATUS2_SUBSTITUTE || aiData->abilities[battlerDef] == ABILITY_STICKY_HOLD)
                            break;
                        else if (ItemId_GetPocket(aiData->items[battlerDef]) == POCKET_BERRIES)
                            ADJUST_SCORE(DECENT_EFFECT);
                        break;
                    case MOVE_EFFECT_INCINERATE:
                        if (gBattleMons[battlerDef].status2 & STATUS2_SUBSTITUTE || aiData->abilities[battlerDef] == ABILITY_STICKY_HOLD)
                            break;
                        else if (ItemId_GetPocket(aiData->items[battlerDef]) == POCKET_BERRIES || aiData->holdEffects[battlerDef] == HOLD_EFFECT_GEMS)
                            ADJUST_SCORE(DECENT_EFFECT);
                        break;
                    case MOVE_EFFECT_SMACK_DOWN:
                        if (!IsBattlerGrounded(battlerDef) && HasDamagingMoveOfType(battlerAtk, TYPE_GROUND) && !CanTargetFaintAi(battlerDef, battlerAtk))
                            ADJUST_SCORE(DECENT_EFFECT);
                        break;
                    case MOVE_EFFECT_KNOCK_OFF:
                        if (CanKnockOffItem(battlerDef, aiData->items[battlerDef]))
                        {
                            switch (aiData->holdEffects[battlerDef])
                            {
                            case HOLD_EFFECT_IRON_BALL:
                                if (HasMoveEffect(battlerDef, EFFECT_FLING))
                                    ADJUST_SCORE(DECENT_EFFECT);
                                break;
                            case HOLD_EFFECT_LAGGING_TAIL:
                            case HOLD_EFFECT_STICKY_BARB:
                                break;
                            default:
                                ADJUST_SCORE(DECENT_EFFECT);
                                break;
                            }
                        }
                        break;
                    case MOVE_EFFECT_STEAL_ITEM:
                        {
                            bool32 canSteal = FALSE;

                            if (B_TRAINERS_KNOCK_OFF_ITEMS == TRUE)
                                canSteal = TRUE;
                            if (gBattleTypeFlags & BATTLE_TYPE_FRONTIER || GetBattlerSide(battlerAtk) == B_SIDE_PLAYER)
                                canSteal = TRUE;

                            if (canSteal && aiData->items[battlerAtk] == ITEM_NONE
                            && aiData->items[battlerDef] != ITEM_NONE
                            && CanBattlerGetOrLoseItem(battlerDef, aiData->items[battlerDef])
                            && CanBattlerGetOrLoseItem(battlerAtk, aiData->items[battlerDef])
                            && !HasMoveEffect(battlerAtk, EFFECT_ACROBATICS)
                            && aiData->abilities[battlerDef] != ABILITY_STICKY_HOLD)
                            {
                                switch (aiData->holdEffects[battlerDef])
                                {
                                case HOLD_EFFECT_NONE:
                                    break;
                                case HOLD_EFFECT_CHOICE_BAND:
                                case HOLD_EFFECT_CHOICE_SCARF:
                                case HOLD_EFFECT_CHOICE_SPECS:
                                    ADJUST_SCORE(DECENT_EFFECT);
                                    break;
                                case HOLD_EFFECT_TOXIC_ORB:
                                    if (ShouldPoisonSelf(battlerAtk, aiData->abilities[battlerAtk]))
                                        ADJUST_SCORE(DECENT_EFFECT);
                                    break;
                                case HOLD_EFFECT_FLAME_ORB:
                                    if (ShouldBurnSelf(battlerAtk, aiData->abilities[battlerAtk]))
                                        ADJUST_SCORE(DECENT_EFFECT);
                                    break;
                                case HOLD_EFFECT_BLACK_SLUDGE:
                                    if (IS_BATTLER_OF_TYPE(battlerAtk, TYPE_POISON))
                                        ADJUST_SCORE(DECENT_EFFECT);
                                    break;
                                case HOLD_EFFECT_IRON_BALL:
                                    if (HasMoveEffect(battlerAtk, EFFECT_FLING))
                                        ADJUST_SCORE(DECENT_EFFECT);
                                    break;
                                case HOLD_EFFECT_LAGGING_TAIL:
                                case HOLD_EFFECT_STICKY_BARB:
                                    break;
                                default:
                                    ADJUST_SCORE(WEAK_EFFECT);
                                    break;
                                }
                            }
                            break;
                        }
                        break;
                    case MOVE_EFFECT_STEALTH_ROCK:
                    case MOVE_EFFECT_SPIKES:
                        if (AI_ShouldSetUpHazards(battlerAtk, battlerDef, aiData));
                        {
                            if (gDisableStructs[battlerAtk].isFirstTurn)
                                ADJUST_SCORE(BEST_EFFECT);
                            else
                                ADJUST_SCORE(DECENT_EFFECT);
                        }
                        break;
                    case MOVE_EFFECT_FEINT:
                        if (GetMoveEffect(predictedMove) == EFFECT_PROTECT)
                            ADJUST_SCORE(GOOD_EFFECT);
                        break;
                    case MOVE_EFFECT_THROAT_CHOP:
                        if (IsSoundMove(GetBestDmgMoveFromBattler(battlerDef, battlerAtk)))
                        {
                            if (AI_IsFaster(battlerAtk, battlerDef, move))
                                ADJUST_SCORE(BEST_EFFECT);
                            else
                                ADJUST_SCORE(GOOD_EFFECT);
                        }
                        // DebugPrintf("CurrentScore1: %d", score);
                        break;
                    case MOVE_EFFECT_WRAP:
                        if (!HasMoveEffect(battlerDef, EFFECT_RAPID_SPIN) && ShouldTrap(battlerAtk, battlerDef, move))
                            ADJUST_SCORE(BEST_EFFECT);
                        break;
                    case MOVE_EFFECT_SALT_CURE:
                        if (IS_BATTLER_OF_TYPE(battlerDef, TYPE_WATER) || IS_BATTLER_OF_TYPE(battlerDef, TYPE_STEEL))
                            ADJUST_SCORE(DECENT_EFFECT);
                        break;
                    case MOVE_EFFECT_DIRE_CLAW:
                        if (AI_CanPoison(battlerAtk, battlerDef, abilityDef, move, MOVE_NONE) 
                        || AI_CanParalyze(battlerAtk, battlerDef, abilityDef, move, MOVE_NONE) 
                        || AI_CanPutToSleep(battlerAtk, battlerDef, abilityDef, move, MOVE_NONE))
                            ADJUST_SCORE(2);
                        break;
                }
            }
        }
        else
        {
            // DebugPrintf("Non-Guaranteed additional score: %d", score);
            if (additionalEffect->self && IsSelfStatLoweringEffect(additionalEffect->moveEffect))
                ADJUST_SCORE(-1);
            else
            {
                switch (additionalEffect->moveEffect)
                {
                    case MOVE_EFFECT_FLINCH:
                        if (ShouldTryToFlinch(battlerAtk, battlerDef, aiData->abilities[battlerAtk], aiData->abilities[battlerDef], move) > 0)
                            ADJUST_SCORE(1);
                        break;
                    case MOVE_EFFECT_SPD_MINUS_1:
                    case MOVE_EFFECT_SPD_MINUS_2:
                    case MOVE_EFFECT_ATK_MINUS_1:
                    case MOVE_EFFECT_DEF_MINUS_1:
                    case MOVE_EFFECT_SP_ATK_MINUS_1:
                    case MOVE_EFFECT_SP_DEF_MINUS_1:
                    case MOVE_EFFECT_ACC_MINUS_1:
                    case MOVE_EFFECT_EVS_MINUS_1:
                        if (aiData->abilities[battlerDef] != ABILITY_CONTRARY)
                            ADJUST_SCORE(1);
                        break;
                    case MOVE_EFFECT_ATK_MINUS_2:
                    case MOVE_EFFECT_DEF_MINUS_2:
                    case MOVE_EFFECT_SP_ATK_MINUS_2:
                    case MOVE_EFFECT_SP_DEF_MINUS_2:
                    case MOVE_EFFECT_ACC_MINUS_2:
                    case MOVE_EFFECT_EVS_MINUS_2:
                        if (aiData->abilities[battlerDef] != ABILITY_CONTRARY)
                            ADJUST_SCORE(1);
                        break;
                    case MOVE_EFFECT_POISON:
                        ADJUST_SCORE(1);
                        break;
                    case MOVE_EFFECT_DIRE_CLAW:
                        if (AI_CanPoison(battlerAtk, battlerDef, abilityDef, move, MOVE_NONE) 
                        || AI_CanParalyze(battlerAtk, battlerDef, abilityDef, move, MOVE_NONE) 
                        || AI_CanPutToSleep(battlerAtk, battlerDef, abilityDef, move, MOVE_NONE))
                            ADJUST_SCORE(1);
                        break;            
                }
            }
        }
        // DebugPrintf("What the fuck score?: %d", score);

        // Consider move effects that target self
    }

    return score;
}

// AI_FLAG_CHECK_VIABILITY - Chooses best possible move to hit player
static s32 AI_CheckViability(u32 battlerAtk, u32 battlerDef, u32 move, s32 score)
{
    // DebugPrintf("Score at the start of CheckViability: %d", score);
    if (GetMovePower(move) != 0)
    {
        if (GetNoOfHitsToKOBattler(battlerAtk, battlerDef, AI_THINKING_STRUCT->movesetIndex) == 0)
            ADJUST_AND_RETURN_SCORE(-20); // No point in checking the move further so return early
        else
        {
            // DebugPrintf("Num hits to ko? %d", GetNoOfHitsToKOBattler(battlerAtk, battlerDef, AI_THINKING_STRUCT->movesetIndex));
            u32 scoreAdjust = AI_CompareDamagingMoves(battlerAtk, battlerDef, AI_THINKING_STRUCT->movesetIndex);
            u32 moveTotalAccuracy = GetTotalAccuracy(battlerAtk, battlerDef, move, AI_DATA->abilities[battlerAtk], AI_DATA->abilities[battlerDef], AI_DATA->holdEffects[battlerAtk], AI_DATA->holdEffects[battlerDef]);
            // DebugPrintf("MoveTotalAccuracy: %d", moveTotalAccuracy);
            if (moveTotalAccuracy <= 50 && moveTotalAccuracy > 0)
                scoreAdjust -= 3;
            else if (moveTotalAccuracy <=70 && moveTotalAccuracy > 0)
                scoreAdjust -= 2;
            else if (moveTotalAccuracy < 100 && moveTotalAccuracy > 0)
                scoreAdjust -= 1;
            
            // DebugPrintf("scoreAdjust: %d", scoreAdjust);
            ADJUST_SCORE(scoreAdjust);
        }
    }
    // DebugPrintf("Score before effect scores: %d", score);

    u32 effectScore = AI_CalcMoveEffectScore(battlerAtk, battlerDef, move);
    // DebugPrintf("effectScore: %d", effectScore);
    ADJUST_SCORE(effectScore);
    // DebugPrintf("Additional Effect Score: %d", score);
    u32 holdScore = AI_CalcHoldEffectMoveScore(battlerAtk, battlerDef, move);
    ADJUST_SCORE(holdScore);

    return score;
}

// Dynamic AI Functions
// For specific battle scenarios

void ScriptSetDynamicAiFunc(struct ScriptContext *ctx)
{
    Script_RequestEffects(SCREFF_V1);

    AiScoreFunc func = (AiScoreFunc)ScriptReadWord(ctx);
    sDynamicAiFunc = func;
}

void ResetDynamicAiFunc(void)
{
    sDynamicAiFunc = NULL;
}
