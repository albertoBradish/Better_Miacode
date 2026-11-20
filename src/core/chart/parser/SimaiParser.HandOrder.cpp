// Included by SimaiParser.cpp after the base parser helpers.

using HandOrderAnnotation = miacode::hand_order::Annotation;

void appendHandOrderError(ParseState* state, int line, int col, int endCol, const QString& detail)
{
    if (state == nullptr) {
        return;
    }
    SimaiMessage error;
    error.line = line;
    error.col = qMax(1, col);
    error.endCol = qMax(error.col, endCol);
    error.message = QStringLiteral("Invalid hand-order annotation: ") + detail;
    state->result.annotationErrors.append(error);
}

HandOrderAnnotation parseHandOrderAnnotation(
    ParseState* state,
    const QString& token,
    int line,
    int column,
    QString* baseToken)
{
    if (baseToken == nullptr) return {};
    const auto parts = miacode::hand_order::splitToken(token);
    *baseToken = parts.baseToken;
    if (parts.suffixOffset < 0) return {};
    const HandOrderAnnotation annotation = miacode::hand_order::parseAnnotation(
        token, tokenContainsSlideShape(*baseToken));
    if (!annotation.error.isEmpty()) {
        appendHandOrderError(state, line, column + annotation.startOffset,
            column + token.size() - 1, annotation.error);
    }
    return annotation;
}

void applyHandOrderAnnotation(
    ParseState* state,
    const HandOrderAnnotation& annotation,
    int firstMarker,
    int line,
    int column,
    int tokenLength)
{
    if (state == nullptr || !annotation.valid) {
        return;
    }
    const int annotationCol = column + annotation.startOffset;
    const int annotationEndCol = column + tokenLength - 1;
    for (int i = firstMarker; i < state->result.noteMarkers.size(); ++i) {
        const auto& marker = state->result.noteMarkers.at(i);
        const bool slide = marker.type == QStringLiteral("slide") || marker.type == QStringLiteral("wifi");
        if ((slide && (marker.slideBranchIndex < 0 || marker.slideBranchIndex >= annotation.branchTracks.size()))
            || (!slide && !annotation.branchTracks.isEmpty())
            || (marker.type != QStringLiteral("wifi")
                && annotation.wifiModes.value(marker.slideBranchIndex, HandOrderWifiMode::Legacy) != HandOrderWifiMode::Legacy)) {
            appendHandOrderError(state, line, annotationCol, annotationEndCol,
                QStringLiteral("track mode does not match the native note or branch"));
            return;
        }
    }
    for (int i = firstMarker; i < state->result.noteMarkers.size(); ++i) {
        TimelineNoteMarker& marker = state->result.noteMarkers[i];
        marker.handOrderAnnotationCol = annotationCol;
        marker.handOrderAnnotationEndCol = annotationEndCol;
        const bool isSlide = marker.type == QStringLiteral("slide")
            || marker.type == QStringLiteral("wifi");
        if (isSlide) {
            marker.handOrderHead = annotation.head;
            marker.handOrderTrackStops = annotation.branchTracks.at(marker.slideBranchIndex);
            marker.handOrderWifiMode = annotation.wifiModes.value(marker.slideBranchIndex, HandOrderWifiMode::Legacy);
            marker.handOrderWifiSectorStops = annotation.wifiSectors.value(marker.slideBranchIndex);
        } else {
            marker.handOrderHead = annotation.head;
        }
    }
}
