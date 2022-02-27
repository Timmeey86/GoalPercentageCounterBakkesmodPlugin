#include <pch.h>
#include "StatFileReader.h"
#include "StatFileDefs.h"

#include <sstream>
#include <filesystem>
#include <fstream>

StatFileReader::StatFileReader(std::shared_ptr<GameWrapper> gameWrapper, std::shared_ptr<ShotDistributionTracker> shotDistributionTracker)
	: _gameWrapper(gameWrapper)
	, _shotDistributionTracker(shotDistributionTracker)
{
}

template<typename T>
int indexInVector(const std::vector<T>& vector, T searchValue)
{
	auto iterator = std::find(vector.begin(), vector.end(), searchValue);
	if (iterator == std::end(vector))
	{
		return -1;
	}
	else
	{
		return (int)std::distance(vector.begin(), iterator);
	}
}

std::vector<std::string> StatFileReader::getAvailableResourcePaths(const std::string& trainingPackCode)
{
	// Read the folder for the current training pack
	std::ostringstream stringStream;
	stringStream << _gameWrapper->GetBakkesModPath().u8string() << u8"\\data\\CustomTrainingStatistics\\" << trainingPackCode;
	auto folderPath = std::filesystem::u8path(stringStream.str());

	std::vector<std::string> filePaths;
	if (std::filesystem::exists(folderPath))
	{
		try
		{
			for (const auto& entry : std::filesystem::directory_iterator(folderPath))
			{
				if (entry.is_regular_file())
				{
					filePaths.emplace_back(entry.path().u8string());
				}
			}
		}
		catch (const std::filesystem::filesystem_error&)
		{
			// treat this case like there would be no files
		}
	}

	// Sort the file paths in descending order so the one with the most recent date appears first
	std::sort(filePaths.begin(), filePaths.end(), std::greater<>());
	return filePaths;
}

std::pair<std::string, std::string> getLineValues(const std::string& line)
{
	std::string delimiter = "\t";

	auto end = line.find(delimiter);
	if (end == std::string::npos)
	{
		return {};
	}

	return { line.substr(0, end), line.substr(end + 1, line.size() - end + 1) };
}


int StatFileReader::peekAttemptAmount(const std::string& resourcePath)
{
	// Try opening the file
	std::ifstream fileStream(resourcePath);
	if (fileStream.fail()) { return 0; }

	// Try reading the version from the file
	std::string currentLine;
	if (!std::getline(fileStream, currentLine)) { return 0; }

	auto [versionTag, versionNumber] = getLineValues(currentLine);
	auto versionIndex = indexInVector(StatFileDefs::SupportedVersionNumbers, versionNumber);
	if (versionTag != StatFileDefs::Version || versionIndex < 0)
	{
		return 0; // Version number is unknown or file is invalid
	}

	// This looks like an actually supported file => Currently, the version will be followed by the amount of shots, a separator line and then the attempts
	// We're only interested in the attempts here

	for (int i = 0; i < 3; i++)
	{
		if (!std::getline(fileStream, currentLine)) { return 0; }
	}

	auto [attemptsTag, attemptAmount] = getLineValues(currentLine);
	auto attempts = std::stoi(attemptAmount);
	if (attemptsTag != StatFileDefs::Attempts || attempts < 0)
	{
		// The file is invalid, maybe someone messed with it
		return 0;
	}

	return attempts;
}

bool readValueIntoField(std::ifstream& stream, int* valuePointer)
{
	std::string currentLine;
	if (!std::getline(stream, currentLine)) { return false; }
	if (currentLine.empty()) { return false; }

	auto [key, value] = getLineValues(currentLine);
	if (key.empty() || value.empty()) { return false; }

	auto valueAsInt = std::stoi(value);
	if (valueAsInt < 0)
	{
		return false;
	}
	*valuePointer = valueAsInt;
	return true;
}
bool readValueIntoField(std::ifstream& stream, double* valuePointer)
{
	std::string currentLine;
	if (!std::getline(stream, currentLine)) { return false; }
	if (currentLine.empty()) { return false; }

	auto [key, value] = getLineValues(currentLine);
	if (key.empty() || value.empty()) { return false; }

	auto valueAsFloat = std::stod(value);
	if (valueAsFloat < .0)
	{
		return false;
	}
	*valuePointer = valueAsFloat;
	return true;
}
bool readValueIntoField(std::ifstream& stream, float* valuePointer)
{
	double tmp;
	auto readSucceeded = readValueIntoField(stream, &tmp);
	if (readSucceeded)
	{
		*valuePointer = (float)tmp;
		return true;
	}
	return false;
}


ShotStats StatFileReader::readStats(const std::string& resourcePath)
{
	// Try opening the file
	std::ifstream fileStream(resourcePath);
	if (fileStream.fail()) { return {}; }

	std::string currentLine;

	// Check for the version number
	if (!std::getline(fileStream, currentLine)) { return {}; }
	auto [versionTag, versionNumber] = getLineValues(currentLine);
	auto versionIndex = indexInVector(StatFileDefs::SupportedVersionNumbers, versionNumber);
	if (versionIndex < 0)
	{
		return {}; // Version number is unknown or file is invalid
	}

	// Actually read the number of shots
	if (!std::getline(fileStream, currentLine)) { return {}; }

	auto [numberOfShotsTag, numberOfShotsValue] = getLineValues(currentLine);
	auto numberOfShots = std::stoi(numberOfShotsValue);
	if (numberOfShotsTag != StatFileDefs::NumberOfShots || numberOfShots <= 0)
	{
		return {};
	}

	// Build a vector which points to the all stats object and then the shot stats objects. That way we can read everything in a loop
	ShotStats stats;
	for (int shotNumber = 0; shotNumber < numberOfShots; shotNumber++)
	{
		stats.PerShotStats.emplace_back();
	}

	std::vector<StatsData*> statsDataObjectsToBeFilled = { &stats.AllShotStats };
	for (int shotNumber = 0; shotNumber < numberOfShots; shotNumber++)
	{
		statsDataObjectsToBeFilled.emplace_back(&stats.PerShotStats[shotNumber]);
	}

	// Lazy approach: We got the version and this looks like a valid file, so we ignore the labels
	//                A more robust approach would obviously be to create a map of key,value pairs and then distribute stats based on that
	for (int shotNumber = -1; shotNumber < numberOfShots; shotNumber++)
	{
		auto vectorIndex = shotNumber + 1;
		auto statsDataPointer = statsDataObjectsToBeFilled[vectorIndex];

		// Skip the dashes lines
		if (!std::getline(fileStream, currentLine)) { return {}; }

		// Read stats
		if (!readVersion_1_0(fileStream, statsDataPointer)) { return {}; }
		if (versionIndex > 0 && !readVersion_1_1_additions(fileStream, statsDataPointer)) { return {}; }
		if (versionIndex > 1 && !readVersion_1_2_additions(fileStream)) { return {}; }
	}
	return stats;
}

bool StatFileReader::readVersion_1_0(std::ifstream& fileStream, StatsData* const statsDataPointer)
{
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.Attempts)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.Goals)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.InitialHits)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.GoalStreakCounter)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.MissStreakCounter)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.LongestGoalStreak)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.LongestMissStreak)) { return false; }

	// last N shot: we need to add true/false values for each 1/0 in the string
	std::string currentLine;
	if (!std::getline(fileStream, currentLine)) { return false; }
	auto [key, boolArrayString] = getLineValues(currentLine);
	// Note: boolArrayString might be empty if the last session didn't include at least one of the shots

	for (int index = 0; index < boolArrayString.size(); index++)
	{
		statsDataPointer->Stats.Last50Shots.emplace_back((boolArrayString.at(index) == '1' ? true : false));
	}

	// We can't restore goal speeds (we would have to export all the single goal values in order to properly restore mean/median)
	if (!std::getline(fileStream, currentLine)) { return false; } // latest speed
	if (!std::getline(fileStream, currentLine)) { return false; } // max speed
	if (!std::getline(fileStream, currentLine)) { return false; } // min speed
	if (!std::getline(fileStream, currentLine)) { return false; } // median speed
	if (!std::getline(fileStream, currentLine)) { return false; } // mean speed

	if (!readValueIntoField(fileStream, &statsDataPointer->Data.InitialHitPercentage)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Data.SuccessPercentage)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Data.PeakSuccessPercentage)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Data.PeakShotNumber)) { return false; }

	return true;
}

bool StatFileReader::readVersion_1_1_additions(std::ifstream& fileStream, StatsData* const statsDataPointer)
{
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.MaxAirDribbleTouches)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.MaxAirDribbleTime)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.MaxGroundDribbleTime)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.DoubleTapGoals)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Data.DoubleTapGoalPercentage)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.MaxFlipResets)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.TotalFlipResets)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Data.AverageFlipResetsPerAttempt)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Data.FlipResetGoalPercentage)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Stats.CloseMisses)) { return false; }
	if (!readValueIntoField(fileStream, &statsDataPointer->Data.CloseMissPercentage)) { return false; }

	return true;
}

bool StatFileReader::readVersion_1_2_additions(std::ifstream& fileStream)
{
	std::string currentLine;
	if (!std::getline(fileStream, currentLine)) { return false; } // This line will contain the whole vector
	if (currentLine.empty()) { return false; }

	auto [key, allShotLocations] = getLineValues(currentLine);
	if (key.empty() || allShotLocations.empty()) { return false; }

	if (key != StatFileDefs::ImpactLocations) { return false; }

	const char separator = '|';
	// read until the first separator

	auto separatorPos = allShotLocations.find(separator);
	if (separatorPos == std::string::npos) { return false; }

	// Try to read the size of the vector	
	if (auto size = std::stoi(allShotLocations.substr(0, separatorPos)); size > 0)
	{
		size_t offset = separatorPos + 1;
		for (int index = 0; index < size; index++)
		{
			// Parse a vector string like "3.456,2.3154,9.223"
			auto valueSeparatorPos = allShotLocations.find(",", offset);
			if (valueSeparatorPos == std::string::npos) { return false; }

			Vector vector;
			vector.X = std::stof(allShotLocations.substr(offset, valueSeparatorPos));
			offset = valueSeparatorPos + 1;

			valueSeparatorPos = allShotLocations.find(",", offset);
			if (valueSeparatorPos == std::string::npos) { return false; }

			vector.Y = std::stof(allShotLocations.substr(offset, valueSeparatorPos));
			offset = valueSeparatorPos + 1;

			valueSeparatorPos = allShotLocations.find(separator, offset);
			if (valueSeparatorPos == std::string::npos) { return false; }

			vector.Z = std::stof(allShotLocations.substr(offset, valueSeparatorPos));
			offset = valueSeparatorPos + 1;

			// restore both impact locations and heatmap by simulating the impacts in the same order
			_shotDistributionTracker->registerImpactLocation(vector);
		}
	}
	// Else: Size 0 is valid, this just means none of the attempts hit the wall or the goal (will be rare)

	return true;
}
