#include "global.h"
#include "test/battle.h"

ASSUMPTIONS
{
    ASSUME(gItemsInfo[ITEM_CHOICE_SPECS].holdEffect == HOLD_EFFECT_CHOICE_SPECS);
    ASSUME(gItemsInfo[ITEM_CHOICE_BAND].holdEffect == HOLD_EFFECT_CHOICE_BAND);
    ASSUME(gItemsInfo[ITEM_CHOICE_SCARF].holdEffect == HOLD_EFFECT_CHOICE_SCARF);
}

AI_SINGLE_BATTLE_TEST("Choiced Pokémon only consider their own status moves when determining if they should switch")
{
    GIVEN 
    {
        AI_FLAGS(AI_FLAG_CHECK_BAD_MOVE | AI_FLAG_TRY_TO_FAINT | AI_FLAG_CHECK_VIABILITY | AI_FLAG_RISKY | AI_FLAG_SMART_SWITCHING | AI_FLAG_OMNISCIENT | AI_FLAG_SMART_MON_CHOICES);
        PLAYER(SPECIES_ZIGZAGOON) { Speed(4); Moves(MOVE_TAIL_WHIP, MOVE_SCRATCH); }
        OPPONENT(SPECIES_ZIGZAGOON) { Speed(5); Moves(MOVE_SCRATCH); Item(ITEM_CHOICE_BAND); }
        OPPONENT(SPECIES_ZIGZAGOON) { Speed(5); Moves(MOVE_SCRATCH); }
    } WHEN {
        TURN { EXPECT_MOVE(opponent, MOVE_SCRATCH); MOVE(player, MOVE_TAIL_WHIP); }
        TURN { EXPECT_MOVE(opponent, MOVE_SCRATCH); MOVE(player, MOVE_TAIL_WHIP); }
    }
}

AI_SINGLE_BATTLE_TEST("Choiced Pokémon won't use stat boosting moves")
{
    // Moves defined by MOVE_TARGET_USER (with exceptions?)
    u32 j, ability = ABILITY_NONE, heldItem = ITEM_NONE;

    static const u32 choiceItems[] = {
        ITEM_CHOICE_SPECS,
        ITEM_CHOICE_BAND,
        ITEM_CHOICE_SCARF,
    };

    for (j = 0; j < ARRAY_COUNT(choiceItems); j++)
    {
        PARAMETRIZE { ability = ABILITY_NONE;    heldItem = choiceItems[j]; }
        PARAMETRIZE { ability = ABILITY_KLUTZ;   heldItem = choiceItems[j]; }
    }

    GIVEN {
        ASSUME(GetMoveTarget(MOVE_SWORDS_DANCE) == MOVE_TARGET_USER);
        AI_FLAGS(AI_FLAG_CHECK_BAD_MOVE | AI_FLAG_CHECK_VIABILITY | AI_FLAG_TRY_TO_FAINT);
        PLAYER(SPECIES_RHYDON)
        OPPONENT(SPECIES_LOPUNNY) { Moves(MOVE_SWORDS_DANCE, MOVE_SCRATCH); Item(heldItem); Ability(ability); }
        OPPONENT(SPECIES_SWAMPERT) { Moves(MOVE_WATERFALL); }
    } WHEN {
        if (ability == ABILITY_KLUTZ) { // Klutz ignores item
            TURN { EXPECT_MOVE(opponent, MOVE_SWORDS_DANCE); }
        }
        else {
            TURN { EXPECT_MOVE(opponent, MOVE_SCRATCH); }
        }
    }
}

AI_SINGLE_BATTLE_TEST("Choiced Pokémon won't use status move if they are the only party member")
{
    u32 j, ability = ABILITY_NONE, isAlive = 0, heldItem = ITEM_NONE;

    static const u32 choiceItems[] = {
        ITEM_CHOICE_SPECS,
        ITEM_CHOICE_BAND,
        ITEM_CHOICE_SCARF,
    };

    for (j = 0; j < ARRAY_COUNT(choiceItems); j++)
    {
        PARAMETRIZE { ability = ABILITY_NONE;    heldItem = choiceItems[j]; isAlive = 0; }
        PARAMETRIZE { ability = ABILITY_KLUTZ;   heldItem = choiceItems[j]; isAlive = 0; }
        PARAMETRIZE { ability = ABILITY_NONE;    heldItem = choiceItems[j]; isAlive = 1; }
        PARAMETRIZE { ability = ABILITY_KLUTZ;   heldItem = choiceItems[j]; isAlive = 1; }
    }

    GIVEN {
        PLAYER(SPECIES_RHYDON)
        OPPONENT(SPECIES_LOPUNNY) { Moves(MOVE_SPORE, MOVE_SCRATCH); Item(heldItem); Ability(ability); }
        OPPONENT(SPECIES_SWAMPERT) { HP(isAlive); Moves(MOVE_WATERFALL); }
    } WHEN {
        if (isAlive == 1 || ability == ABILITY_KLUTZ) {
            TURN { EXPECT_MOVE(opponent, MOVE_SPORE); }
        }
        else {
            TURN { EXPECT_MOVE(opponent, MOVE_SCRATCH); }
        }
    }
}

AI_SINGLE_BATTLE_TEST("Choiced Pokémon won't use status move if they are trapped")
{
    u32 j, aiAbility = ABILITY_NONE, playerAbility = MOVE_NONE, species = SPECIES_NONE, heldItem = ITEM_NONE;

    static const u32 choiceItems[] = {
        ITEM_CHOICE_SPECS,
        ITEM_CHOICE_BAND,
        ITEM_CHOICE_SCARF,
    };

    for (j = 0; j < ARRAY_COUNT(choiceItems); j++)
    {
        PARAMETRIZE { aiAbility = ABILITY_NONE;    heldItem = choiceItems[j]; species = SPECIES_RHYDON; playerAbility = ABILITY_LIGHTNING_ROD; }
        PARAMETRIZE { aiAbility = ABILITY_KLUTZ;   heldItem = choiceItems[j]; species = SPECIES_RHYDON; playerAbility = ABILITY_LIGHTNING_ROD; }
        PARAMETRIZE { aiAbility = ABILITY_NONE;    heldItem = choiceItems[j]; species = SPECIES_DUGTRIO; playerAbility = ABILITY_ARENA_TRAP; }
        PARAMETRIZE { aiAbility = ABILITY_KLUTZ;   heldItem = choiceItems[j]; species = SPECIES_DUGTRIO; playerAbility = ABILITY_ARENA_TRAP; }
    }

    GIVEN {
        PLAYER(species) { Ability(playerAbility); }
        OPPONENT(SPECIES_LOPUNNY) { Moves(MOVE_SPORE, MOVE_SCRATCH); Item(heldItem); Ability(aiAbility); }
        OPPONENT(SPECIES_SWAMPERT) { Moves(MOVE_WATERFALL); }
    } WHEN {
        if (playerAbility != ABILITY_ARENA_TRAP || aiAbility == ABILITY_KLUTZ) {
            TURN { EXPECT_MOVE(opponent, MOVE_SPORE); }
        }
        else {
            TURN { EXPECT_MOVE(opponent, MOVE_SCRATCH); }
        }
    }
}
