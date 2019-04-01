/******************************************************************************
 *
 * Copyright (c) 2017, the Perspective Authors.
 *
 * This file is part of the Perspective library, distributed under the terms of
 * the Apache License 2.0.  The full license can be found in the LICENSE file.
 *
 */

import * as d3 from "d3";
import {seriesColorRange} from "../series/seriesRange";

export function treeData(settings) {
    const sets = {};
    settings.data.forEach(d => {
        const groups = d.__ROW_PATH__;
        const splits = getSplitNames(d);
        splits.forEach(split => {
            let currentLevel;
            if (!sets[split]) {
                sets[split] = [];
            }
            currentLevel = sets[split];
            groups.forEach((group, i) => {
                let element = currentLevel.find(e => e.name === group);
                if (!element) {
                    element = {name: group, children: []};
                    currentLevel.push(element);
                }
                if (settings.mainValues.length > 1) {
                    const colorValue = getDataValue(d, settings.mainValues[1], split);
                    element.color = element.color ? element.color + colorValue : colorValue;
                }
                if (i === groups.length - 1) {
                    element.name = groups.slice(-1)[0];
                    if (groups.length === settings.crossValues.length) {
                        element.size = getDataValue(d, settings.mainValues[0], split);
                    }
                }
                currentLevel = element.children;
            });
        });
    });

    const data = Object.entries(sets).map(set => {
        const tree = {name: "root", children: set[1]};
        const root = d3.hierarchy(tree).sum(d => d.size);
        const color = treeColor(settings, set);
        return {split: set[0], data: d3.partition().size([2 * Math.PI, root.height + 1])(root), color};
    });

    return data;
}

const getDataValue = (d, aggregate, split) => (split.length ? d[`${split}|${aggregate.name}`] : d[aggregate.name]);

function getSplitNames(d) {
    const splits = [];
    Object.keys(d).forEach(key => {
        if (key !== "__ROW_PATH__") {
            const splitValue = key
                .split("|")
                .slice(0, -1)
                .join("|");
            if (!splits.includes(splitValue)) {
                splits.push(splitValue);
            }
        }
    });
    return splits;
}

function treeColor(settings, [split, data]) {
    if (settings.mainValues.length > 1) {
        const min = Math.min(...settings.data.map(d => getDataValue(d, settings.mainValues[1], split)));
        const max = Math.max(...data.map(d => d.color));
        return seriesColorRange(settings, null, null, [min, max]);
    }
    return () => "rgb(31, 119, 180)";
}
