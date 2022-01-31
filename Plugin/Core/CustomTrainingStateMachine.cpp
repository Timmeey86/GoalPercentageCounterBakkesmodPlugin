#include <pch.h>
#include "CustomTrainingStateMachine.h"

#include <sstream>
#include <iosfwd>

CustomTrainingStateMachine::CustomTrainingStateMachine(
	std::shared_ptr<CVarManagerWrapper> cvarManager, 
	std::shared_ptr<IStatUpdater> statUpdater, 
	std::shared_ptr<PluginState> pluginState)
	: _cvarManager( cvarManager )
	, _statUpdater( statUpdater )
	, _pluginState( pluginState )
{
	_currentState = CustomTrainingState::NotInCustomTraining;
}

void CustomTrainingStateMachine::hookToEvents(const std::shared_ptr<GameWrapper>& gameWrapper)
{
	// TODO: Reset the state machine to the current shot if the plugin is reloaded while already in custom training
	// TODO: Do a final calculation when entering the training result screen, or going back to the main menu. 
	//       That way, the summary window will be up to date when it gets opened after that (or is already open)

	// Happens whenever a goal was scored
	gameWrapper->HookEvent("Function TAGame.Ball_TA.OnHitGoal", [this, gameWrapper](const std::string&) {
		if (!gameWrapper->IsInCustomTraining()) { return; }

		auto gameServer = gameWrapper->GetGameEventAsServer();
		if (gameServer.IsNull()) { return; }
		auto ball = gameServer.GetBall();
		if (ball.IsNull()) { return; }

		_pluginState->setBallSpeed(ball.GetVelocity().magnitude());
		processOnHitGoal();
	});

	// Happens whenever the ball is being touched
	gameWrapper->HookEvent("Function TAGame.Ball_TA.OnCarTouch", [this, gameWrapper](const std::string&) {
		if (!gameWrapper->IsInCustomTraining()) { return; }

		processOnCarTouch();
	});

	// Happens whenever a button was pressed after loading a new shot
	gameWrapper->HookEvent("Function TAGame.TrainingEditorMetrics_TA.TrainingShotAttempt", [this, gameWrapper](const std::string&) {
		if (!gameWrapper->IsInCustomTraining()) { return; }

		processTrainingShotAttempt();
	});

	// Happens whenever a shot is changed or loaded in custom training
	gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.GameEvent_TrainingEditor_TA.EventRoundChanged",
		[this, gameWrapper](ActorWrapper caller, void*, const std::string&) {
		if (!gameWrapper->IsInCustomTraining()) { return; }

		TrainingEditorWrapper trainingWrapper(caller.memory_address);
		processsEventRoundChanged(trainingWrapper);
	});

	// Happens whenever the current custom training map gets unloaded, e.g. because of leaving to the main menu or loading a different training pack
	gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.GameEvent_TrainingEditor_TA.Destroyed",
		[this, gameWrapper](ActorWrapper caller, void*, const std::string&) {

		// Finish the current attempt if an attempt was started, otherwise ignore the event
		if (_currentState == CustomTrainingState::AttemptInProgress)
		{
			TrainingEditorWrapper trainingWrapper(caller.memory_address);
			processsEventRoundChanged(trainingWrapper);
		}
	});

	// Note: The calling class hooks into OnTrainingModeLoaded
}

void CustomTrainingStateMachine::processOnTrainingModeLoaded(TrainingEditorWrapper& trainingWrapper)
{
	// Jump to the resetting state from whereever we were before - it doesn't matter since we reset everything anyway
	setCurrentState(CustomTrainingState::Resetting);
	_pluginState->TotalRounds = trainingWrapper.GetTotalRounds();
	_pluginState->CurrentRoundIndex = -1;

	// The player reloaded the same, or loaded a different training pack => Reset statistics
	_statUpdater->processReset(_pluginState->TotalRounds);
}

void CustomTrainingStateMachine::processsEventRoundChanged(TrainingEditorWrapper& trainingWrapper)
{
	auto newRoundIndex = trainingWrapper.GetActiveRoundNumber();
	if (_currentState == CustomTrainingState::Resetting)
	{
		// Automatic event after loading a training pack => Nothing special to be done
		setCurrentState(CustomTrainingState::PreparingNewShot);
	}
	else if (_currentState == CustomTrainingState::PreparingNewShot)
	{
		// The player must have switched to a different shot before starting their attempt
		if (_pluginState->CurrentRoundIndex == newRoundIndex)
		{
			// This could be a bug in the state machine: The player can't press reset before starting a new attempt, and can't switch to the same shot
			_cvarManager->log("[Custom Training State Machine] [WARNING] Detected an unexpected shot reset before starting an attempt.");
		}
	}
	else if (_currentState == CustomTrainingState::AttemptInProgress)
	{
		if (_goalWasScoredInCurrentAttempt)
		{
			// Temporarily enter pseudo state "Processing Goal"
			setCurrentState(CustomTrainingState::ProcessingGoal);
			_statUpdater->processGoal();
		}
		else
		{
			// Temporarily enter pseudo state "Processing Miss"
			setCurrentState(CustomTrainingState::ProcessingMiss);
			_statUpdater->processMiss();
		}
		
		// Automatically transition to the next state after updating calculations
		_statUpdater->updateData();
		setCurrentState(CustomTrainingState::PreparingNewShot);
	}
	// Else: Ignore the event. This e.g. happens before OnTrainingModeLoaded

	_pluginState->CurrentRoundIndex = newRoundIndex;
}

void CustomTrainingStateMachine::processTrainingShotAttempt()
{
#if DEBUG_STATE_MACHINE
	if (_currentState != CustomTrainingState::PreparingNewShot)
	{
		_cvarManager->log("[Custom Training State Machine] [WARNING] Ignoring TrainingShotAttempt event while in " + to_string(_currentState));
		return;
	}
#endif

	setCurrentState(CustomTrainingState::AttemptInProgress);
	_goalWasScoredInCurrentAttempt = false;
	_ballWasHitInCurrentAttempt = false;
	_statUpdater->processAttempt();
}

void CustomTrainingStateMachine::processOnCarTouch()
{
	if (!_ballWasHitInCurrentAttempt)
	{
		_ballWasHitInCurrentAttempt = true;
		_statUpdater->processInitialBallHit();
	}
	// else: The ball was hit more often, or we are in goal replay => ignore the event
}

void CustomTrainingStateMachine::processOnHitGoal()
{
	if (!_goalWasScoredInCurrentAttempt)
	{
		_goalWasScoredInCurrentAttempt = true;

		// Note: We do not process the goal yet. This will happen when leaving the current state
	}
	// else: We are most likely in goal replay => ignore the event
}

void CustomTrainingStateMachine::setCurrentState(CustomTrainingState newState)
{
#if DEBUG_STATE_MACHINE
	std::ostringstream stream;
	stream << "[Custom Training State Machine] Transitioning from '" << to_string(_currentState) << "' to '" << to_string(newState) << "'";
	_cvarManager->log(stream.str());
#endif
	_currentState = newState;
}
