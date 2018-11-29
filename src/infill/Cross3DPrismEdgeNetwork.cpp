/** Copyright (C) 2018 Ultimaker - Released under terms of the AGPLv3 License */
#include "Cross3DPrismEdgeNetwork.h"
#include "../utils/math.h"
#include "../utils/linearAlg2D.h"


namespace cura {


Cross3DPrismEdgeNetwork::Cross3DPrismEdgeNetwork(const Cross3D& subdivision_structure)
: subdivision_structure(&subdivision_structure)
{
    TimeKeeper tk;
    std::vector<std::vector<Cell*>> depth_ordered_cells = const_cast<Cross3D*>(&subdivision_structure)->getDepthOrdered();
    for (int_fast32_t depth_idx = depth_ordered_cells.size() - 1; depth_idx >= 0; depth_idx--)
    {
        for (const Cell* cell : depth_ordered_cells[depth_idx])
        {
            addCellEdges(*cell);
        }
    }
    for (std::vector<Cell*>& cells : depth_ordered_cells)
    {
        for (const Cell* cell : cells)
        {
            preventZdiscontinuityProblem(*cell, Cross3D::Direction::UP);
            preventZdiscontinuityProblem(*cell, Cross3D::Direction::DOWN);
        }
    }
    logDebug("Created edge network in %5.2fs.\n");
#ifdef DEBUG
    debugCheckInclinations();
#endif
}

char Cross3DPrismEdgeNetwork::getNeighborDepth(const Cell& cell, Cross3D::Direction direction)
{
    const std::list<Cross3D::Link>& neighbors = cell.adjacent_cells[static_cast<size_t>(direction)];
    if (neighbors.empty())
    {
        return std::numeric_limits<char>::max();
    }
    return subdivision_structure->cell_data[neighbors.front().to_index].depth;
}

void Cross3DPrismEdgeNetwork::addCellEdges(const Cell& cell)
{
    if (cell.depth > getNeighborDepth(cell, Cross3D::Direction::LEFT))
    {
        addCellEdge(cell, cell_to_left_edge_locations, Cross3D::Direction::LEFT);
    }
    if (cell.depth >= getNeighborDepth(cell, Cross3D::Direction::RIGHT))
    {
        addCellEdge(cell, cell_to_right_edge_locations, Cross3D::Direction::RIGHT);
    }
}

void Cross3DPrismEdgeNetwork::addCellEdge(const Cell& cell, std::map<const Cross3D::Cell*, std::vector<Point3>>& cell_to_edge_locations, Cross3D::Direction edge_side)
{
    LineSegment edge = (edge_side == Cross3D::Direction::LEFT)? cell.elem.triangle.getFromEdge() : cell.elem.triangle.getToEdge();

    std::vector<Point3> edge_locations;
    if (!cell.elem.is_expanding)
    {
        std::swap(edge.from, edge.to);
    }
    edge_locations.emplace_back(edge.from.X, edge.from.Y, cell.elem.z_range.min);
    edge_locations.emplace_back(edge.to.X, edge.to.Y, cell.elem.z_range.max);

    applyOscillationConstraints(cell, edge_side, Cross3D::Direction::UP, edge_locations);
    applyOscillationConstraints(cell, edge_side, Cross3D::Direction::DOWN, edge_locations);

    cell_to_edge_locations.emplace(&cell, edge_locations);
}

void Cross3DPrismEdgeNetwork::applyOscillationConstraints(const Cell& cell, Cross3D::Direction edge_side, Cross3D::Direction up_down, std::vector<Point3>& edge_locations)
{
    // naming here assumes up_down is UP
    std::vector<Point3>* edge_above = nullptr;
    { // get the edge_above
        const std::list<Cross3D::Link>& upstairs_neighbors = cell.adjacent_cells[static_cast<size_t>(up_down)];
        if (upstairs_neighbors.empty())
        {
            return; // no oscillation constraints for the bottom layer of cells, nor for the top
        }
        const size_t upstairs_neighbor_idx = (edge_side == Cross3D::Direction::LEFT)? upstairs_neighbors.front().to_index : upstairs_neighbors.back().to_index;
        const Cell& upstairs_neighbor = subdivision_structure->cell_data[upstairs_neighbor_idx];
        const std::list<Cross3D::Link>& upstairs_side_neighbors = upstairs_neighbor.adjacent_cells[static_cast<size_t>(edge_side)];
        assert(!upstairs_side_neighbors.empty());
        const size_t upstairs_side_neighbor_idx = (up_down == Cross3D::Direction::UP)? upstairs_side_neighbors.front().to_index : upstairs_side_neighbors.back().to_index;
        const Cell& upstairs_side_neighbor = subdivision_structure->cell_data[upstairs_side_neighbor_idx];
        if (cell.depth >= std::max(upstairs_neighbor.depth, upstairs_side_neighbor.depth))
        {
            return; // upstairs neighbor doesnt constrain this edge
        }
        if (upstairs_neighbor.depth > upstairs_side_neighbor.depth
            || (edge_side == Cross3D::Direction::RIGHT && upstairs_neighbor.depth == upstairs_side_neighbor.depth)) // upstairs neighbor is on the left of the other upstairs neighbor and their depth is equal
        {
            auto edge_above_iterator = ((edge_side == Cross3D::Direction::LEFT)? cell_to_left_edge_locations : cell_to_right_edge_locations).find(&upstairs_neighbor);
            assert(edge_above_iterator != cell_to_left_edge_locations.end() && edge_above_iterator != cell_to_right_edge_locations.end());
            edge_above = &edge_above_iterator->second;
        }
        else
        {
            auto edge_above_iterator = ((edge_side == Cross3D::Direction::RIGHT)? cell_to_left_edge_locations : cell_to_right_edge_locations).find(&upstairs_side_neighbor);
            assert(edge_above_iterator != cell_to_left_edge_locations.end() && edge_above_iterator != cell_to_right_edge_locations.end());
            edge_above = &edge_above_iterator->second;
        }
        assert(edge_above);
    }
    assert(!edge_above->empty());
    const Point3 move_destination = (up_down == Cross3D::Direction::UP)? edge_above->front() : edge_above->back();

    adjustEdgeEnd(edge_locations, up_down, move_destination);
}

std::vector<Point3>& Cross3DPrismEdgeNetwork::getEdge(const Cell& cell, Cross3D::Direction edge_side, Cross3D::Direction up_down)
{
    const std::list<Cross3D::Link>& neighbors = cell.adjacent_cells[static_cast<size_t>(edge_side)];
    const Cell& neighbor = subdivision_structure->cell_data[((up_down == Cross3D::Direction::UP)? neighbors.back() : neighbors.front()).to_index];
    if (neighbor.depth > cell.depth ||
        (edge_side == Cross3D::Direction::LEFT && neighbor.depth == cell.depth))
    {
        //                                                             get left edge of right cell    get right edge of left cell
        auto edge_iterator = ((edge_side == Cross3D::Direction::RIGHT)? cell_to_left_edge_locations : cell_to_right_edge_locations).find(&neighbor);
        assert(edge_iterator != cell_to_left_edge_locations.end() && edge_iterator != cell_to_right_edge_locations.end());
        return edge_iterator->second;
    }
    else
    {
        //                                                             get left edge of right cell    get right edge of left cell
        auto edge_iterator = ((edge_side == Cross3D::Direction::LEFT)? cell_to_left_edge_locations : cell_to_right_edge_locations).find(&cell);
        assert(edge_iterator != cell_to_left_edge_locations.end() && edge_iterator != cell_to_right_edge_locations.end());
        return edge_iterator->second;
    }
}

void Cross3DPrismEdgeNetwork::preventZdiscontinuityProblem(const Cell& cell, Cross3D::Direction up_down)
{
    // naming here assumes up_down is UP
    const std::list<Cross3D::Link>& upstairs_neighbors = cell.adjacent_cells[static_cast<size_t>(up_down)];
    if (upstairs_neighbors.size() < 2)
    {
        return; // the cell above doesn't introduce a discontinuity
    }

    // the line segment crossing [cell] at the top/bottom of the cell
    const std::vector<Point3>& from_edge = getEdge(cell, Cross3D::Direction::LEFT, up_down);
    Point from = toPoint((up_down == Direction::UP)? from_edge.back() : from_edge.front());
    const std::vector<Point3>& to_edge = getEdge(cell, Cross3D::Direction::RIGHT, up_down);
    Point to = toPoint((up_down == Direction::UP)? to_edge.back() : to_edge.front());

    // both cells above it are the same depth, so the left one (front) is the owner of the edge in between which cuases the problem
    const Cell& leftmost_upstairs_neighbor = subdivision_structure->cell_data[upstairs_neighbors.front().to_index];
    const Cell& rightmost_upstairs_neighbor = subdivision_structure->cell_data[upstairs_neighbors.back().to_index];
    assert(leftmost_upstairs_neighbor.depth == cell.depth + 1);
    assert(rightmost_upstairs_neighbor.depth == leftmost_upstairs_neighbor.depth);
    assert(cell_to_left_edge_locations.find(&rightmost_upstairs_neighbor) == cell_to_left_edge_locations.end() && "leftmost cell must be owner of both cells have smae depth");

    auto trouble_edge_iterator = cell_to_right_edge_locations.find(&leftmost_upstairs_neighbor);
    assert(trouble_edge_iterator != cell_to_right_edge_locations.end());
    std::vector<Point3>& trouble_edge_locations = trouble_edge_iterator->second;

    LineSegment trouble_edge = leftmost_upstairs_neighbor.elem.triangle.getToEdge();
    Point trouble_edge_middle = LinearAlg2D::intersection(trouble_edge, LineSegment(from, to));
    Point3 trouble_edge_middle_3d = toPoint3(trouble_edge_middle, (up_down == Cross3D::Direction::UP)? leftmost_upstairs_neighbor.elem.z_range.min : leftmost_upstairs_neighbor.elem.z_range.max);

    adjustEdgeEnd(trouble_edge_locations, Cross3D::opposite(up_down), trouble_edge_middle_3d);
}

void Cross3DPrismEdgeNetwork::adjustEdgeEnd(std::vector<Point3>& edge_locations, Cross3D::Direction up_down, Point3 move_destination)
{
    Point3& to_be_moved = (up_down == Cross3D::Direction::UP)? edge_locations.back() : edge_locations.front();
    if (to_be_moved == move_destination)
    {
        return;
    }
    const Point3 lower_edge_location = (up_down == Cross3D::Direction::UP)? edge_locations[edge_locations.size() - 2] : edge_locations[1];
    const coord_t move_length = vSize(toPoint(move_destination) - toPoint(to_be_moved));
    const Point3 edge_direction_vector = lower_edge_location - to_be_moved;
    const coord_t edge_direction_vector_length = vSize(toPoint(edge_direction_vector));
    const Point3 bending_point = to_be_moved + edge_direction_vector * move_length / 2 / edge_direction_vector_length;

    if ((bending_point - lower_edge_location).vSize2() > 10 * 10
        && (bending_point - move_destination).vSize2() > 10 * 10)
    {
        to_be_moved = move_destination;
        edge_locations.insert(edge_locations.begin() + ((up_down == Cross3D::Direction::UP)? edge_locations.size() - 1 : 1), bending_point);
    }
    else
    {
        to_be_moved = move_destination;
    }
}

Point Cross3DPrismEdgeNetwork::getCellEdgeLocation(const Cross3D::Cell& before, const Cross3D::Cell& after, const coord_t z) const
{
    auto edge_locations_iterator = (after.depth > before.depth)? cell_to_left_edge_locations.find(&after) : cell_to_right_edge_locations.find(&before);
    assert(edge_locations_iterator != cell_to_left_edge_locations.end() && edge_locations_iterator != cell_to_right_edge_locations.end());
    const std::vector<Point3>& edge_locations = edge_locations_iterator->second;
    assert(edge_locations.size() >= 2);
    for (size_t edge_location_idx = 0; edge_location_idx < edge_locations.size(); edge_location_idx++)
    {
        if (z <= edge_locations[edge_location_idx + 1].z)
        {
            const Point3 edge_location_below = edge_locations[edge_location_idx];
            const Point3 edge_location_above = edge_locations[edge_location_idx + 1];
            const coord_t rest_z = z - edge_location_below.z;
            const Point edge_direction(edge_location_above.x - edge_location_below.x, edge_location_above.y - edge_location_below.y);
            assert(edge_direction != Point(0, 0));
            return Point(edge_location_below.x, edge_location_below.y) + edge_direction * rest_z / (edge_location_above.z - edge_location_below.z);
        }
    }
    assert(false && "this function should only be called when z coord lies somewhere on the edge!");
}


void Cross3DPrismEdgeNetwork::debugCheckInclinations() const
{
    for (auto pair : cell_to_left_edge_locations)
    {
        debugCheckInclination(pair.second);
    }
    for (auto pair : cell_to_right_edge_locations)
    {
        debugCheckInclination(pair.second);
    }
}

void Cross3DPrismEdgeNetwork::debugCheckInclination(const std::vector<Point3>& edge)
{
    assert(edge.size() > 1);
    Point3 below = edge.front();
    for (size_t idx = 1; idx < edge.size(); idx++)
    {
        const Point3 above = edge[idx];
        coord_t xy = vSize(toPoint(above - below));
        coord_t z = above.z - below.z;
        float angle = atan(INT2MM(z) / INT2MM(xy));
        float angle_deg = angle / M_PI * 180;
        assert(angle_deg > 35.0);
        below = above;
    }
}

}; // namespace cura
