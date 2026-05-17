#include <myengine/editor/EditorCommandHistory.h>

namespace myengine::editor
{
    LambdaEditorCommand::LambdaEditorCommand(std::string label, Operation undoOperation, Operation redoOperation)
        : label_(std::move(label)),
          undoOperation_(std::move(undoOperation)),
          redoOperation_(std::move(redoOperation))
    {
    }

    bool LambdaEditorCommand::Undo()
    {
        return undoOperation_ != nullptr ? undoOperation_() : false;
    }

    bool LambdaEditorCommand::Redo()
    {
        return redoOperation_ != nullptr ? redoOperation_() : false;
    }

    const std::string& LambdaEditorCommand::Label() const
    {
        return label_;
    }

    void EditorCommandHistory::Clear()
    {
        undoStack_.clear();
        redoStack_.clear();
    }

    bool EditorCommandHistory::CanUndo() const
    {
        return !undoStack_.empty();
    }

    bool EditorCommandHistory::CanRedo() const
    {
        return !redoStack_.empty();
    }

    bool EditorCommandHistory::Undo()
    {
        if (undoStack_.empty())
        {
            return false;
        }

        std::unique_ptr<IEditorCommand> command = std::move(undoStack_.back());
        undoStack_.pop_back();

        if (!command->Undo())
        {
            return false;
        }

        redoStack_.push_back(std::move(command));
        return true;
    }

    bool EditorCommandHistory::Redo()
    {
        if (redoStack_.empty())
        {
            return false;
        }

        std::unique_ptr<IEditorCommand> command = std::move(redoStack_.back());
        redoStack_.pop_back();

        if (!command->Redo())
        {
            return false;
        }

        undoStack_.push_back(std::move(command));
        return true;
    }

    void EditorCommandHistory::Push(std::unique_ptr<IEditorCommand> command)
    {
        if (command == nullptr)
        {
            return;
        }

        redoStack_.clear();
        undoStack_.push_back(std::move(command));
    }
}